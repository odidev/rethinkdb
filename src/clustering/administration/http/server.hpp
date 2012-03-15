#ifndef __CLUSTERING_ADMINISTRATION_HTTP_SERVER_HPP__
#define __CLUSTERING_ADMINISTRATION_HTTP_SERVER_HPP__

#include "clustering/administration/metadata.hpp"
#include "http/http.hpp"
#include "rpc/directory/read_view.hpp"
#include "rpc/semilattice/view.hpp"

class http_server_t;
class routing_http_app_t;
class file_http_app_t;
class semilattice_http_app_t;
class directory_http_app_t;

class administrative_http_server_manager_t {

public:
    administrative_http_server_manager_t(
        int port,
        boost::shared_ptr<semilattice_readwrite_view_t<cluster_semilattice_metadata_t> > _semilattice_metadata, 
        clone_ptr_t<directory_rview_t<cluster_directory_metadata_t> > _directory_metadata,
        boost::uuids::uuid _us);
    ~administrative_http_server_manager_t();

private:
    boost::scoped_ptr<file_http_app_t> file_app;
    boost::scoped_ptr<semilattice_http_app_t> semilattice_app;
    boost::scoped_ptr<directory_http_app_t> directory_app;
    boost::scoped_ptr<routing_http_app_t> ajax_routing_app;
    boost::scoped_ptr<routing_http_app_t> root_routing_app;
    boost::scoped_ptr<http_server_t> server;
};

#endif /* __CLUSTERING_ADMINISTRATION_HTTP_SERVER_HPP__ */
