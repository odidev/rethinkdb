#ifndef PROTOCOL_API_HPP_
#define PROTOCOL_API_HPP_

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "concurrency/fifo_checker.hpp"
#include "concurrency/fifo_enforcer.hpp"
#include "concurrency/rwi_lock.hpp"
#include "concurrency/signal.hpp"
#include "containers/binary_blob.hpp"
#include "containers/scoped.hpp"
#include "rpc/serialize_macros.hpp"
#include "timestamps.hpp"

namespace boost {
template <class> class function;
template <class> class scoped_ptr;
}

/* This file describes the relationship between the protocol-specific logic for
each protocol and the protocol-agnostic logic that routes queries for all the
protocols. Each protocol defines a `protocol_t` struct that acts as a
"container" to hold all the types for that protocol. The protocol-agnostic logic
will be templatized on a `protocol_t`. `protocol_t` itself is never
instantiated. For a description of what `protocol_t` must be like, see
`rethinkdb/docs_internal/protocol_api_notes.hpp`. */

class cannot_perform_query_exc_t : public std::exception {
public:
    explicit cannot_perform_query_exc_t(const std::string &s) : message(s) { }
    ~cannot_perform_query_exc_t() throw () { }
    const char *what() const throw () {
        return message.c_str();
    }
private:
    std::string message;
};

/* `namespace_interface_t` is the interface that the protocol-agnostic database
logic for query routing exposes to the protocol-specific query parser. */

template<class protocol_t>
class namespace_interface_t {
public:
    virtual typename protocol_t::read_response_t read(typename protocol_t::read_t, order_token_t tok, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t, cannot_perform_query_exc_t) = 0;
    virtual typename protocol_t::read_response_t read_outdated(typename protocol_t::read_t, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t, cannot_perform_query_exc_t) = 0;
    virtual typename protocol_t::write_response_t write(typename protocol_t::write_t, order_token_t tok, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t, cannot_perform_query_exc_t) = 0;

    /* These calls are for the sole purpose of optimizing queries; don't rely
    on them for correctness. They should not block. */
    virtual std::set<typename protocol_t::region_t> get_sharding_scheme() THROWS_ONLY(cannot_perform_query_exc_t) {
        /* Valid default implementation */
        std::set<typename protocol_t::region_t> s;
        s.insert(protocol_t::region_t::universe());
        return s;
    }

protected:
    virtual ~namespace_interface_t() { }
};

/* Some `protocol_t::region_t` functions can be implemented in terms of other
functions. Here are default implementations for those functions. */

template<class region_t>
bool region_is_empty(const region_t &r) {
    return region_is_superset(region_t::empty(), r);
}

// TODO: you're probably being lazy and should write faster code if
// you're using this function (and maybe you're temporarily a bad
// person).
template<class region_t>
bool region_overlaps(const region_t &r1, const region_t &r2) {
    return !region_is_empty(region_intersection(r1, r2));
}

/* Regions contained in region_map_t must never intersect. */
template<class protocol_t, class value_t>
class region_map_t {
private:
    typedef std::pair<typename protocol_t::region_t, value_t> internal_pair_t;
    typedef std::vector<internal_pair_t> internal_vec_t;
public:
    typedef typename internal_vec_t::const_iterator const_iterator;
    typedef typename internal_vec_t::iterator iterator;

    region_map_t() THROWS_NOTHING {
        regions_and_values.push_back(internal_pair_t(protocol_t::region_t::universe(), value_t()));
    }

    explicit region_map_t(typename protocol_t::region_t r, value_t v = value_t()) THROWS_NOTHING {
        regions_and_values.push_back(internal_pair_t(r, v));
    }

    template <class input_iterator_t>
    region_map_t(const input_iterator_t &_begin, const input_iterator_t &_end)
        : regions_and_values(_begin, _end)
    {
        DEBUG_ONLY(get_domain());
    }

public:
    typename protocol_t::region_t get_domain() const THROWS_NOTHING {
        std::vector<typename protocol_t::region_t> regions;
        for (const_iterator it = begin(); it != end(); it++) {
            regions.push_back(it->first);
        }
        typename protocol_t::region_t join;
        DEBUG_ONLY_VAR region_join_result_t join_result = region_join(regions, &join);
        guarantee(join_result == REGION_JOIN_OK);
        return join;
    }

    const_iterator begin() const {
        return regions_and_values.begin();
    }

    const_iterator end() const {
        return regions_and_values.end();
    }

    iterator begin() {
        return regions_and_values.begin();
    }

    iterator end() {
        return regions_and_values.end();
    }

    MUST_USE region_map_t mask(typename protocol_t::region_t region) const {
        internal_vec_t masked_pairs;
        for (int i = 0; i < (int)regions_and_values.size(); i++) {
            typename protocol_t::region_t ixn = region_intersection(regions_and_values[i].first, region);
            if (!region_is_empty(ixn)) {
                masked_pairs.push_back(internal_pair_t(ixn, regions_and_values[i].second));
            }
        }
        return region_map_t(masked_pairs.begin(), masked_pairs.end());
    }

    // Important: 'update' assumes that new_values regions do not intersect
    void update(const region_map_t& new_values) {
        rassert(region_is_superset(get_domain(), new_values.get_domain()), "Update cannot expand the domain of a region_map.");
        std::vector<typename protocol_t::region_t> overlay_regions;
        for (const_iterator i = new_values.begin(); i != new_values.end(); ++i) {
            overlay_regions.push_back((*i).first);
        }

        internal_vec_t updated_pairs;
        for (const_iterator i = begin(); i != end(); ++i) {
            typename protocol_t::region_t old = (*i).first;
            std::vector<typename protocol_t::region_t> old_subregions = region_subtract_many(old, overlay_regions);

            // Insert the unchanged parts of the old region into updated_pairs with the old value
            for (typename std::vector<typename protocol_t::region_t>::const_iterator j = old_subregions.begin(); j != old_subregions.end(); ++j) {
                updated_pairs.push_back(internal_pair_t(*j, (*i).second));
            }
        }
        std::copy(new_values.begin(), new_values.end(), std::back_inserter(updated_pairs));

        regions_and_values = updated_pairs;
    }

    void set(const typename protocol_t::region_t &r, const value_t &v) {
        update(region_map_t(r, v));
    }

    size_t size() const {
        return regions_and_values.size();
    }

private:
    internal_vec_t regions_and_values;
    RDB_MAKE_ME_SERIALIZABLE_1(regions_and_values);
};

template <class P, class V>
void debug_print(append_only_printf_buffer_t *buf, const region_map_t<P, V> &map) {
    buf->appendf("rmap{");
    for (typename region_map_t<P, V>::const_iterator it = map.begin(); it != map.end(); ++it) {
        if (it != map.begin()) {
            buf->appendf(", ");
        }
        debug_print(buf, it->first);
        buf->appendf(" => ");
        debug_print(buf, it->second);
    }
    buf->appendf("}");
}

template<class P, class V>
bool operator==(const region_map_t<P, V> &left, const region_map_t<P, V> &right) {
    if (left.get_domain() != right.get_domain()) {
        return false;
    }

    for (typename region_map_t<P, V>::const_iterator i = left.begin(); i != left.end(); ++i) {
        region_map_t<P, V> r = right.mask((*i).first);
        for (typename region_map_t<P, V>::const_iterator j = r.begin(); j != r.end(); ++j) {
            if (j->second != i->second) {
                return false;
            }
        }
    }
    return true;
}

template<class P, class V>
bool operator!=(const region_map_t<P, V> &left, const region_map_t<P, V> &right) {
    return !(left == right);
}

template<class protocol_t, class old_t, class new_t, class callable_t>
region_map_t<protocol_t, new_t> region_map_transform(const region_map_t<protocol_t, old_t> &original, const callable_t &callable) {
    std::vector<std::pair<typename protocol_t::region_t, new_t> > new_pairs;
    for (typename region_map_t<protocol_t, old_t>::const_iterator it =  original.begin();
                                                                  it != original.end();
                                                                  it++) {
        new_pairs.push_back(std::pair<typename protocol_t::region_t, new_t>(
                it->first,
                callable(it->second)
                ));
    }
    return region_map_t<protocol_t, new_t>(new_pairs.begin(), new_pairs.end());
}

#ifndef NDEBUG
// Checks that the metainfo has a certain value, or certain kind of value.
template <class protocol_t>
class metainfo_checker_callback_t {
public:
    virtual void check_metainfo(const region_map_t<protocol_t, binary_blob_t>& metainfo,
                                const typename protocol_t::region_t& domain) const = 0;
protected:
    metainfo_checker_callback_t() { }
    virtual ~metainfo_checker_callback_t() { }
private:
    DISABLE_COPYING(metainfo_checker_callback_t);
};

template <class protocol_t>
struct trivial_metainfo_checker_callback_t : public metainfo_checker_callback_t<protocol_t> {

    trivial_metainfo_checker_callback_t() { }
    void check_metainfo(UNUSED const region_map_t<protocol_t, binary_blob_t>& metainfo, UNUSED const typename protocol_t::region_t& region) const {
        /* do nothing */
    }

private:
    DISABLE_COPYING(trivial_metainfo_checker_callback_t);
};

template <class protocol_t>
class metainfo_checker_t {
public:
    metainfo_checker_t(const metainfo_checker_callback_t<protocol_t> *callback,
                       const typename protocol_t::region_t& region) : callback_(callback), region_(region) { }

    void check_metainfo(const region_map_t<protocol_t, binary_blob_t>& metainfo) const {
        callback_->check_metainfo(metainfo, region_);
    }
    const typename protocol_t::region_t& get_domain() const { return region_; }
    const metainfo_checker_t mask(const typename protocol_t::region_t& region) const {
        return metainfo_checker_t(callback_, region_intersection(region, region_));
    }

private:
    const metainfo_checker_callback_t<protocol_t> *const callback_;
    const typename protocol_t::region_t region_;

    // This _is_ copyable because of mask, but all copies' lifetimes
    // are limited by that of callback_.
};

#endif  // NDEBUG

/* `store_view_t` is an abstract class that represents a region of a key-value
store for some protocol. It's templatized on the protocol (`protocol_t`). It
covers some `protocol_t::region_t`, which is returned by `get_region()`.

In addition to the actual data, `store_view_t` is responsible for keeping track
of metadata which is keyed by region. The metadata is currently implemented as
opaque binary blob (`binary_blob_t`).
*/

template <class protocol_t>
class store_view_t : public home_thread_mixin_t {
public:
    typedef region_map_t<protocol_t, binary_blob_t> metainfo_t;

    virtual ~store_view_t() {
        assert_thread();
    }

    typename protocol_t::region_t get_region() {
        return region;
    }

    virtual void new_read_token(scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token_out) = 0;
    virtual void new_write_token(scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token_out) = 0;

    /* Gets the metainfo.
    [Postcondition] return_value.get_domain() == view->get_region()
    [May block] */
    virtual metainfo_t get_metainfo(order_token_t order_token,
                                    scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token,
                                    signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) = 0;

    /* Replaces the metainfo over the view's entire range with the given metainfo.
    [Precondition] region_is_superset(view->get_region(), new_metainfo.get_domain())
    [Postcondition] this->get_metainfo() == new_metainfo
    [May block] */
    virtual void set_metainfo(const metainfo_t &new_metainfo,
                              order_token_t order_token,
                              scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
                              signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) = 0;

    /* Performs a read.
    [Precondition] region_is_superset(view->get_region(), expected_metainfo.get_domain())
    [Precondition] region_is_superset(expected_metainfo.get_domain(), read.get_region())
    [May block] */
    virtual typename protocol_t::read_response_t read(
            DEBUG_ONLY(const metainfo_checker_t<protocol_t>& metainfo_expecter, )
            const typename protocol_t::read_t &read,
            order_token_t order_token,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) = 0;

    /* Performs a write.
    [Precondition] region_is_superset(view->get_region(), expected_metainfo.get_domain())
    [Precondition] new_metainfo.get_domain() == expected_metainfo.get_domain()
    [Precondition] region_is_superset(expected_metainfo.get_domain(), write.get_region())
    [May block] */
    virtual typename protocol_t::write_response_t write(
            DEBUG_ONLY(const metainfo_checker_t<protocol_t>& metainfo_expecter, )
            const metainfo_t& new_metainfo,
            const typename protocol_t::write_t &write,
            transition_timestamp_t timestamp,
            order_token_t order_token,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) = 0;

    /* Expresses the changes that have happened since `start_point` as a
    series of `backfill_chunk_t` objects.
    [Precondition] start_point.get_domain() <= view->get_region()
    [Side-effect] `should_backfill` must be called exactly once
    [Return value] Value equal to the value returned by should_backfill
    [May block]
    */
    virtual bool send_backfill(
            const region_map_t<protocol_t, state_timestamp_t> &start_point,
            const boost::function<bool(const metainfo_t&)> &should_backfill,
            const boost::function<void(typename protocol_t::backfill_chunk_t)> &chunk_fun,
            typename protocol_t::backfill_progress_t *progress,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) = 0;


    /* Applies a backfill data chunk sent by `send_backfill()`. If
    `interrupted_exc_t` is thrown, the state of the database is undefined
    except that doing a second backfill must put it into a valid state.
    [May block]
    */
    virtual void receive_backfill(
            const typename protocol_t::backfill_chunk_t &chunk,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) = 0;

    /* Deletes every key in the region.
    [Precondition] region_is_superset(region, subregion)
    [May block]
     */
    virtual void reset_data(
            const typename protocol_t::region_t &subregion,
            const metainfo_t &new_metainfo,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) = 0;

protected:
    explicit store_view_t(typename protocol_t::region_t r) : region(r) { }

private:
    typename protocol_t::region_t region;
};

/* The query-routing logic provides the following ordering guarantees:

1.  All the replicas of each individual key will see writes in the same order.

    Example: Suppose K = "x". You send (append "a" to K) and (append "b" to K)
    concurrently from different nodes. Either every copy of K will become "xab",
    or every copy of K will become "xba", but the different copies of K will
    never disagree.

2.  Queries from the same origin will be performed in same order they are sent.

    Example: Suppose K = "a". You send (set K to "b") and (read K) from the same
    thread on the same node, in that order. The read will return "b".

3.  Arbitrary atomic single-key operations can be performed, as long as they can
    be expressed as `protocol_t::write_t` objects.

4.  There are no other atomicity or ordering guarantees.

    Example: Suppose K1 = "x" and K2 = "x". You send (append "a" to every key)
    and (append "b" to every key) concurrently. Every copy of K1 will agree with
    every other copy of K1, and every copy of K2 will agree with every other
    copy of K2, but K1 and K2 may disagree.

    Example: Suppose K = "a". You send (set K to "b"). As soon as it's sent, you
    send (set K to "c") from a different node. K may end up being either "b" or
    "c".

    Example: Suppose K1 = "a" and K2 = "a". You send (set K1 to "b") and (set K2
    to "b") from the same node, in that order. Then you send (read K1 and K2)
    from a different node. The read may return (K1 = "a", K2 = "b").

5.  There is no simple way to perform an atomic multikey transaction. You might
    be able to fake it by using a key as a "lock".
*/

template <class protocol_t>
class store_subview_t : public store_view_t<protocol_t>
{
public:
    typedef typename store_view_t<protocol_t>::metainfo_t metainfo_t;

    store_subview_t(store_view_t<protocol_t> *_store_view, typename protocol_t::region_t region)
        : store_view_t<protocol_t>(region), store_view(_store_view)
    {
        rassert(region_is_superset(_store_view->get_region(), region));
    }

    using store_view_t<protocol_t>::get_region;

    void new_read_token(scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token_out) {
        store_view->new_read_token(token_out);
    }

    void new_write_token(scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token_out) {
        store_view->new_write_token(token_out);
    }

    metainfo_t get_metainfo(order_token_t order_token,
                            scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token,
                            signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        return store_view->get_metainfo(order_token, token, interruptor).mask(get_region());
    }

    void set_metainfo(const metainfo_t &new_metainfo,
                      order_token_t order_token,
                      scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
                      signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(region_is_superset(get_region(), new_metainfo.get_domain()));
        store_view->set_metainfo(new_metainfo, order_token, token, interruptor);
    }

    typename protocol_t::read_response_t read(
            DEBUG_ONLY(const metainfo_checker_t<protocol_t>& metainfo_checker, )
            const typename protocol_t::read_t &read,
            order_token_t order_token,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) {
        rassert(region_is_superset(get_region(), metainfo_checker.get_domain()));

        return store_view->read(DEBUG_ONLY(metainfo_checker, ) read, order_token, token, interruptor);
    }

    typename protocol_t::write_response_t write(
            DEBUG_ONLY(const metainfo_checker_t<protocol_t>& metainfo_checker, )
            const metainfo_t& new_metainfo,
            const typename protocol_t::write_t &write,
            transition_timestamp_t timestamp,
            order_token_t order_token,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) {
        rassert(region_is_superset(get_region(), metainfo_checker.get_domain()));
        rassert(region_is_superset(get_region(), new_metainfo.get_domain()));

        return store_view->write(DEBUG_ONLY(metainfo_checker, ) new_metainfo, write, timestamp, order_token, token, interruptor);
    }

    bool send_backfill(
            const region_map_t<protocol_t, state_timestamp_t> &start_point,
            const boost::function<bool(const metainfo_t&)> &should_backfill,
            const boost::function<void(typename protocol_t::backfill_chunk_t)> &chunk_fun,
            typename protocol_t::backfill_progress_t *p,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_read_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) {
        rassert(region_is_superset(get_region(), start_point.get_domain()));

        return store_view->send_backfill(start_point, should_backfill, chunk_fun, p, token, interruptor);
    }

    void receive_backfill(
            const typename protocol_t::backfill_chunk_t &chunk,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) {
                store_view->receive_backfill(chunk, token, interruptor);
    }

    void reset_data(
            const typename protocol_t::region_t &subregion,
            const metainfo_t &new_metainfo,
            scoped_ptr_t<fifo_enforcer_sink_t::exit_write_t> *token,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) {
        rassert(region_is_superset(get_region(), subregion));
        rassert(region_is_superset(get_region(), new_metainfo.get_domain()));

        store_view->reset_data(subregion, new_metainfo, token, interruptor);
    }

public:
    store_view_t<protocol_t> *store_view;
};

#endif /* PROTOCOL_API_HPP_ */
