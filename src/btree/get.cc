#include "btree/get.hpp"

#include "utils.hpp"
#include "btree/delete_expired.hpp"
#include "btree/internal_node.hpp"
#include "btree/leaf_node.hpp"
#include "buffer_cache/buf_lock.hpp"
#include "buffer_cache/co_functions.hpp"
#include "perfmon.hpp"
#include "btree/coro_wrappers.hpp"
#include "store.hpp"
#include "concurrency/cond_var.hpp"

void co_btree_get(const btree_key *key, btree_slice_t *slice, promise_t<store_t::get_result_t> *res) {
    union {
        char value_memory[MAX_BTREE_VALUE_SIZE];
        btree_value value;
    };
    (void)value_memory;

    block_pm_duration get_time(&pm_cmd_get);

    cache_t *cache = &slice->cache;

    int home_thread = get_thread_id();
    coro_t::move_to_thread(slice->home_thread);

    block_pm_duration get_time_2(&pm_cmd_get_without_threads);

    transactor_t transactor(cache, rwi_read);

    //Acquire the superblock
    buf_lock_t buf_lock(transactor, SUPERBLOCK_ID, rwi_read);

    block_id_t node_id = ptr_cast<btree_superblock_t>(buf_lock.buf()->get_data_read())->root_block;
    rassert(node_id != SUPERBLOCK_ID);

    //Acquire the root
    if (node_id == NULL_BLOCK_ID) {
        // No root exists
        buf_lock.release();

        // Commit transaction now because we won't be returning to this core
        transactor.commit();
        get_time_2.end();
        coro_t::move_to_thread(home_thread);
        co_deliver_get_result(NULL, 0, 0, res);
        return;
    }

    // Acquire the leaf node
    while (true) {
        {
            buf_lock_t tmp(transactor, node_id, rwi_read);
            buf_lock.swap(tmp);
        }

#ifndef NDEBUG
        node::validate(cache->get_block_size(), ptr_cast<node_t>(buf_lock.buf()->get_data_read()));
#endif

        const node_t *node = ptr_cast<node_t>(buf_lock.buf()->get_data_read());
        if (!node::is_internal(node)) {
            break;
        }

        block_id_t next_node_id = internal_node::lookup(ptr_cast<internal_node_t>(node), key);
        rassert(next_node_id != NULL_BLOCK_ID);
        rassert(next_node_id != SUPERBLOCK_ID);

        node_id = next_node_id;
    }


    // Got down to the leaf, now examine it
    bool found = leaf::lookup(ptr_cast<leaf_node_t>(buf_lock.buf()->get_data_read()), key, &value);
    buf_lock.release();

    if (found && value.expired()) {
        btree_delete_expired(key, slice);
        found = false;
    }
    
    /* get() has two paths it takes: one for large values and one for small ones. For large
    values, it holds onto the large value buffer while it goes back to the request handler's core
    and delivers the large value. Then it returns again to the cache's core and frees the value,
    and finally goes to the request handler's core again to free itself. For small values, it
    duplicates the value into an internal buffer and then goes to the request handler's core,
    delivers the value, and frees itself all in one trip. If the value is not found, we take
    a third path that is basically the same as the one for small values. */
    if (!found) {
        // Commit transaction now because we won't be returning to this core
        transactor.commit();

        get_time_2.end();
        coro_t::move_to_thread(home_thread);
        co_deliver_get_result(NULL, 0, 0, res);
        return;
    } else if (value.is_large()) {
        // Don't commit transaction yet because we need to keep holding onto
        // the large buf until it's been read.
        rassert(value.is_large());

        large_buf_t *large_value = new large_buf_t(transactor.transaction());

        co_acquire_large_value(large_value, value.lb_ref(), rwi_read);
        rassert(large_value->state == large_buf_t::loaded);
        rassert(large_value->get_root_ref().block_id == value.lb_ref().block_id);

        const_buffer_group_t value_buffers;
        for (int i = 0; i < large_value->get_num_segments(); i++) {
            uint16_t size;
            const void *data = large_value->get_segment(i, &size);
            value_buffers.add_buffer(size, data);
        }
        get_time_2.end();
        coro_t::move_to_thread(home_thread);
        co_deliver_get_result(&value_buffers, value.mcflags(), 0, res);
        {
            on_thread_t mover(slice->home_thread);
            large_value->release();
            delete large_value;
            transactor.commit();
        }
    } else {
        // Commit transaction now because we won't be returning to this core
        transactor.commit();

        const_buffer_group_t value_buffers;
        value_buffers.add_buffer(value.value_size(), value.value());
        get_time_2.end();
        coro_t::move_to_thread(home_thread);
        co_deliver_get_result(&value_buffers, value.mcflags(), 0, res);
    }
}

store_t::get_result_t btree_get(const btree_key *key, btree_slice_t *slice) {
    promise_t<store_t::get_result_t> res;
    coro_t::spawn(co_btree_get, key, slice, &res);
    return res.wait();
}
