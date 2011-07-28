#ifndef __BTREE_DETEMPLATIZER_HPP__
#define	__BTREE_DETEMPLATIZER_HPP__

/*
 op_name is the name of a template function to call, arguments are the function
 arguments, and leaf_node is a pointer to a leaf_node_t.
 DETEMPLATIZE_LEAF_NODE_OP(op_name, leaf_node, sizer_argument, ...)
 selects a typename T based on the leaf_node's magic and runs the following:
 value_sizer_t<T>(sizer_argument) sizer;
 op_name<T>(&sizer, ...);
 */
#define DETEMPLATIZE_LEAF_NODE_OP(op_name, leaf_node, sizer_argument, ...) \
    do { \
        if (leaf_node->magic == value_sizer_t<memcached_value_t>::btree_leaf_magic()) { \
            value_sizer_t<memcached_value_t> sizer(sizer_argument); \
            op_name<memcached_value_t>(&sizer, __VA_ARGS__); \
        } else if (leaf_node->magic == value_sizer_t<riak_value_t>::btree_leaf_magic()) { \
            value_sizer_t<riak_value_t> sizer(sizer_argument); \
            op_name<riak_value_t>(&sizer, __VA_ARGS__); \
        } else \
            crash("Unmatched leaf node magic: %.*s", (int)sizeof(block_magic_t), leaf_node->magic.bytes); \
    } while(0)

#endif	/* __BTREE_DETEMPLATIZER_HPP__ */

