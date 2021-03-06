/*
 * Copyright (c) 2016-2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vnet/session/application_interface.h>
#include <vnet/session/session.h>
#include <vlibmemory/api.h>

/** @file
    VPP's application/session API bind/unbind/connect/disconnect calls
*/

#define app_interface_check_thread_and_barrier(_fn, _arg)		\
  if (PREDICT_FALSE (!vlib_thread_is_main_w_barrier ()))		\
    {									\
      vlib_rpc_call_main_thread (_fn, (u8 *) _arg, sizeof(*_arg));	\
      return 0;								\
    }

u8
session_endpoint_in_ns (session_endpoint_t * sep)
{
  u8 is_lep = session_endpoint_is_local (sep);
  if (!is_lep && sep->sw_if_index != ENDPOINT_INVALID_INDEX
      && !ip_interface_has_address (sep->sw_if_index, &sep->ip, sep->is_ip4))
    {
      clib_warning ("sw_if_index %u not configured with ip %U",
		    sep->sw_if_index, format_ip46_address, &sep->ip,
		    sep->is_ip4);
      return 0;
    }
  return (is_lep || ip_is_local (sep->fib_index, &sep->ip, sep->is_ip4));
}

int
api_parse_session_handle (u64 handle, u32 * session_index, u32 * thread_index)
{
  session_manager_main_t *smm = vnet_get_session_manager_main ();
  session_t *pool;

  *thread_index = handle & 0xFFFFFFFF;
  *session_index = handle >> 32;

  if (*thread_index >= vec_len (smm->wrk))
    return VNET_API_ERROR_INVALID_VALUE;

  pool = smm->wrk[*thread_index].sessions;

  if (pool_is_free_index (pool, *session_index))
    return VNET_API_ERROR_INVALID_VALUE_2;

  return 0;
}

static void
session_endpoint_update_for_app (session_endpoint_cfg_t * sep,
				 application_t * app, u8 is_connect)
{
  app_namespace_t *app_ns;
  u32 ns_index, fib_index;

  ns_index = app->ns_index;

  /* App is a transport proto, so fetch the calling app's ns */
  if (app->flags & APP_OPTIONS_FLAGS_IS_TRANSPORT_APP)
    {
      app_worker_t *owner_wrk;
      application_t *owner_app;

      owner_wrk = app_worker_get (sep->app_wrk_index);
      owner_app = application_get (owner_wrk->app_index);
      ns_index = owner_app->ns_index;
    }
  app_ns = app_namespace_get (ns_index);
  if (!app_ns)
    return;

  /* Ask transport and network to bind to/connect using local interface
   * that "supports" app's namespace. This will fix our local connection
   * endpoint.
   */

  /* If in default namespace and user requested a fib index use it */
  if (ns_index == 0 && sep->fib_index != ENDPOINT_INVALID_INDEX)
    fib_index = sep->fib_index;
  else
    fib_index = sep->is_ip4 ? app_ns->ip4_fib_index : app_ns->ip6_fib_index;
  sep->peer.fib_index = fib_index;
  sep->fib_index = fib_index;

  if (!is_connect)
    {
      sep->sw_if_index = app_ns->sw_if_index;
    }
  else
    {
      if (app_ns->sw_if_index != APP_NAMESPACE_INVALID_INDEX
	  && sep->peer.sw_if_index != ENDPOINT_INVALID_INDEX
	  && sep->peer.sw_if_index != app_ns->sw_if_index)
	clib_warning ("Local sw_if_index different from app ns sw_if_index");

      sep->peer.sw_if_index = app_ns->sw_if_index;
    }
}

static inline int
vnet_listen_inline (vnet_listen_args_t * a)
{
  app_listener_t *app_listener;
  app_worker_t *app_wrk;
  application_t *app;
  int rv;

  app = application_get_if_valid (a->app_index);
  if (!app)
    return VNET_API_ERROR_APPLICATION_NOT_ATTACHED;

  app_wrk = application_get_worker (app, a->wrk_map_index);
  if (!app_wrk)
    return VNET_API_ERROR_INVALID_VALUE;

  a->sep_ext.app_wrk_index = app_wrk->wrk_index;

  session_endpoint_update_for_app (&a->sep_ext, app, 0 /* is_connect */ );
  if (!session_endpoint_in_ns (&a->sep))
    return VNET_API_ERROR_INVALID_VALUE_2;

  /*
   * Check if we already have an app listener
   */
  app_listener = app_listener_lookup (app, &a->sep_ext);
  if (app_listener)
    {
      if (app_listener->app_index != app->app_index)
	return VNET_API_ERROR_ADDRESS_IN_USE;
      if (app_worker_start_listen (app_wrk, app_listener))
	return -1;
      a->handle = app_listener_handle (app_listener);
      return 0;
    }

  /*
   * Create new app listener
   */
  if ((rv = app_listener_alloc_and_init (app, &a->sep_ext, &app_listener)))
    return rv;

  if ((rv = app_worker_start_listen (app_wrk, app_listener)))
    {
      app_listener_cleanup (app_listener);
      return rv;
    }

  a->handle = app_listener_handle (app_listener);
  return 0;
}

static inline int
vnet_unlisten_inline (vnet_unbind_args_t * a)
{
  app_worker_t *app_wrk;
  app_listener_t *al;
  application_t *app;

  if (!(app = application_get_if_valid (a->app_index)))
    return VNET_API_ERROR_APPLICATION_NOT_ATTACHED;

  al = app_listener_get_w_handle (a->handle);
  if (al->app_index != app->app_index)
    {
      clib_warning ("app doesn't own handle %llu!", a->handle);
      return -1;
    }

  app_wrk = application_get_worker (app, a->wrk_map_index);
  if (!app_wrk)
    {
      clib_warning ("no app %u worker %u", app->app_index, a->wrk_map_index);
      return -1;
    }

  return app_worker_stop_listen (app_wrk, al);
}

static int
application_connect (vnet_connect_args_t * a)
{
  app_worker_t *server_wrk, *client_wrk;
  application_t *client;
  local_session_t *ll;
  app_listener_t *al;
  u32 table_index;
  session_t *ls;
  u8 fib_proto;
  u64 lh;

  if (session_endpoint_is_zero (&a->sep))
    return VNET_API_ERROR_INVALID_VALUE;

  client = application_get (a->app_index);
  session_endpoint_update_for_app (&a->sep_ext, client, 1 /* is_connect */ );
  client_wrk = application_get_worker (client, a->wrk_map_index);

  /*
   * First check the local scope for locally attached destinations.
   * If we have local scope, we pass *all* connects through it since we may
   * have special policy rules even for non-local destinations, think proxy.
   */
  if (application_has_local_scope (client))
    {
      table_index = application_local_session_table (client);
      lh = session_lookup_local_endpoint (table_index, &a->sep);
      if (lh == SESSION_DROP_HANDLE)
	return VNET_API_ERROR_APP_CONNECT_FILTERED;

      if (lh == SESSION_INVALID_HANDLE)
	goto global_scope;

      ll = application_get_local_listener_w_handle (lh);
      al = app_listener_get_w_session ((session_t *) ll);

      /*
       * Break loop if rule in local table points to connecting app. This
       * can happen if client is a generic proxy. Route connect through
       * global table instead.
       */
      if (al->app_index == a->app_index)
	goto global_scope;

      server_wrk = app_listener_select_worker (al);
      return app_worker_local_session_connect (client_wrk, server_wrk, ll,
					       a->api_context);
    }

  /*
   * If nothing found, check the global scope for locally attached
   * destinations. Make sure first that we're allowed to.
   */

global_scope:
  if (session_endpoint_is_local (&a->sep))
    return VNET_API_ERROR_SESSION_CONNECT;

  if (!application_has_global_scope (client))
    return VNET_API_ERROR_APP_CONNECT_SCOPE;

  fib_proto = session_endpoint_fib_proto (&a->sep);
  table_index = application_session_table (client, fib_proto);
  ls = session_lookup_listener (table_index, &a->sep);
  if (ls)
    {
      al = app_listener_get_w_session (ls);
      server_wrk = app_listener_select_worker (al);
      ll = (local_session_t *) ls;
      return app_worker_local_session_connect (client_wrk, server_wrk, ll,
					       a->api_context);
    }

  /*
   * Not connecting to a local server, propagate to transport
   */
  if (app_worker_connect_session (client_wrk, &a->sep, a->api_context))
    return VNET_API_ERROR_SESSION_CONNECT;
  return 0;
}

/**
 * unformat a vnet URI
 *
 * transport-proto://[hostname]ip46-addr:port
 * eg. 	tcp://ip46-addr:port
 * 	tls://[testtsl.fd.io]ip46-addr:port
 *
 * u8 ip46_address[16];
 * u16  port_in_host_byte_order;
 * stream_session_type_t sst;
 * u8 *fifo_name;
 *
 * if (unformat (input, "%U", unformat_vnet_uri, &ip46_address,
 *              &sst, &port, &fifo_name))
 *  etc...
 *
 */
uword
unformat_vnet_uri (unformat_input_t * input, va_list * args)
{
  session_endpoint_cfg_t *sep = va_arg (*args, session_endpoint_cfg_t *);
  u32 transport_proto = 0, port;

  if (unformat (input, "%U://%U/%d", unformat_transport_proto,
		&transport_proto, unformat_ip4_address, &sep->ip.ip4, &port))
    {
      sep->transport_proto = transport_proto;
      sep->port = clib_host_to_net_u16 (port);
      sep->is_ip4 = 1;
      return 1;
    }
  else if (unformat (input, "%U://[%s]%U/%d", unformat_transport_proto,
		     &transport_proto, &sep->hostname, unformat_ip4_address,
		     &sep->ip.ip4, &port))
    {
      sep->transport_proto = transport_proto;
      sep->port = clib_host_to_net_u16 (port);
      sep->is_ip4 = 1;
      return 1;
    }
  else if (unformat (input, "%U://%U/%d", unformat_transport_proto,
		     &transport_proto, unformat_ip6_address, &sep->ip.ip6,
		     &port))
    {
      sep->transport_proto = transport_proto;
      sep->port = clib_host_to_net_u16 (port);
      sep->is_ip4 = 0;
      return 1;
    }
  else if (unformat (input, "%U://[%s]%U/%d", unformat_transport_proto,
		     &transport_proto, &sep->hostname, unformat_ip6_address,
		     &sep->ip.ip6, &port))
    {
      sep->transport_proto = transport_proto;
      sep->port = clib_host_to_net_u16 (port);
      sep->is_ip4 = 0;
      return 1;
    }
  return 0;
}

static u8 *cache_uri;
static session_endpoint_cfg_t *cache_sep;

int
parse_uri (char *uri, session_endpoint_cfg_t * sep)
{
  unformat_input_t _input, *input = &_input;

  if (cache_uri && !strncmp (uri, (char *) cache_uri, vec_len (cache_uri)))
    {
      *sep = *cache_sep;
      return 0;
    }

  /* Make sure */
  uri = (char *) format (0, "%s%c", uri, 0);

  /* Parse uri */
  unformat_init_string (input, uri, strlen (uri));
  if (!unformat (input, "%U", unformat_vnet_uri, sep))
    {
      unformat_free (input);
      return VNET_API_ERROR_INVALID_VALUE;
    }
  unformat_free (input);

  vec_free (cache_uri);
  cache_uri = (u8 *) uri;
  if (cache_sep)
    clib_mem_free (cache_sep);
  cache_sep = clib_mem_alloc (sizeof (*sep));
  *cache_sep = *sep;

  return 0;
}

static int
app_validate_namespace (u8 * namespace_id, u64 secret, u32 * app_ns_index)
{
  app_namespace_t *app_ns;
  if (vec_len (namespace_id) == 0)
    {
      /* Use default namespace */
      *app_ns_index = 0;
      return 0;
    }

  *app_ns_index = app_namespace_index_from_id (namespace_id);
  if (*app_ns_index == APP_NAMESPACE_INVALID_INDEX)
    return VNET_API_ERROR_APP_INVALID_NS;
  app_ns = app_namespace_get (*app_ns_index);
  if (!app_ns)
    return VNET_API_ERROR_APP_INVALID_NS;
  if (app_ns->ns_secret != secret)
    return VNET_API_ERROR_APP_WRONG_NS_SECRET;
  return 0;
}

static u8 *
app_name_from_api_index (u32 api_client_index)
{
  vl_api_registration_t *regp;
  regp = vl_api_client_index_to_registration (api_client_index);
  if (regp)
    return format (0, "%s%c", regp->name, 0);

  clib_warning ("api client index %u does not have an api registration!",
		api_client_index);
  return format (0, "unknown%c", 0);
}

/**
 * Attach application to vpp
 *
 * Allocates a vpp app, i.e., a structure that keeps back pointers
 * to external app and a segment manager for shared memory fifo based
 * communication with the external app.
 */
clib_error_t *
vnet_application_attach (vnet_app_attach_args_t * a)
{
  svm_fifo_segment_private_t *fs;
  application_t *app = 0;
  app_worker_t *app_wrk;
  segment_manager_t *sm;
  u32 app_ns_index = 0;
  u8 *app_name = 0;
  u64 secret;
  int rv;

  if (a->api_client_index != APP_INVALID_INDEX)
    app = application_lookup (a->api_client_index);
  else if (a->name)
    app = application_lookup_name (a->name);
  else
    return clib_error_return_code (0, VNET_API_ERROR_INVALID_VALUE, 0,
				   "api index or name must be provided");

  if (app)
    return clib_error_return_code (0, VNET_API_ERROR_APP_ALREADY_ATTACHED, 0,
				   "app already attached");

  if (a->api_client_index != APP_INVALID_INDEX)
    {
      app_name = app_name_from_api_index (a->api_client_index);
      a->name = app_name;
    }

  secret = a->options[APP_OPTIONS_NAMESPACE_SECRET];
  if ((rv = app_validate_namespace (a->namespace_id, secret, &app_ns_index)))
    return clib_error_return_code (0, rv, 0, "namespace validation: %d", rv);
  a->options[APP_OPTIONS_NAMESPACE] = app_ns_index;

  if ((rv = application_alloc_and_init ((app_init_args_t *) a)))
    return clib_error_return_code (0, rv, 0, "app init: %d", rv);

  app = application_get (a->app_index);
  if ((rv = application_alloc_worker_and_init (app, &app_wrk)))
    return clib_error_return_code (0, rv, 0, "app default wrk init: %d", rv);

  a->app_evt_q = app_wrk->event_queue;
  app_wrk->api_client_index = a->api_client_index;
  sm = segment_manager_get (app_wrk->first_segment_manager);
  fs = segment_manager_get_segment_w_lock (sm, 0);

  if (application_is_proxy (app))
    application_setup_proxy (app);

  ASSERT (vec_len (fs->ssvm.name) <= 128);
  a->segment = &fs->ssvm;
  a->segment_handle = segment_manager_segment_handle (sm, fs);

  segment_manager_segment_reader_unlock (sm);
  vec_free (app_name);
  return 0;
}

/**
 * Detach application from vpp
 */
int
vnet_application_detach (vnet_app_detach_args_t * a)
{
  application_t *app;

  app = application_get_if_valid (a->app_index);
  if (!app)
    {
      clib_warning ("app not attached");
      return VNET_API_ERROR_APPLICATION_NOT_ATTACHED;
    }

  app_interface_check_thread_and_barrier (vnet_application_detach, a);
  application_detach_process (app, a->api_client_index);
  return 0;
}

int
vnet_bind_uri (vnet_listen_args_t * a)
{
  session_endpoint_cfg_t sep = SESSION_ENDPOINT_CFG_NULL;
  int rv;

  rv = parse_uri (a->uri, &sep);
  if (rv)
    return rv;
  sep.app_wrk_index = 0;
  clib_memcpy (&a->sep_ext, &sep, sizeof (sep));
  return vnet_listen_inline (a);
}

int
vnet_unbind_uri (vnet_unbind_args_t * a)
{
  session_endpoint_cfg_t sep = SESSION_ENDPOINT_CFG_NULL;
  session_t *listener;
  u32 table_index;
  int rv;

  rv = parse_uri (a->uri, &sep);
  if (rv)
    return rv;

  /* NOTE: only default fib tables supported for uri apis */
  table_index = session_lookup_get_index_for_fib (fib_ip_proto (!sep.is_ip4),
						  0);
  listener = session_lookup_listener (table_index,
				      (session_endpoint_t *) & sep);
  if (!listener)
    return VNET_API_ERROR_ADDRESS_NOT_IN_USE;
  a->handle = listen_session_get_handle (listener);
  return vnet_unlisten_inline (a);
}

clib_error_t *
vnet_connect_uri (vnet_connect_args_t * a)
{
  session_endpoint_cfg_t sep = SESSION_ENDPOINT_CFG_NULL;
  int rv;

  /* Parse uri */
  rv = parse_uri (a->uri, &sep);
  if (rv)
    return clib_error_return_code (0, rv, 0, "app init: %d", rv);

  clib_memcpy (&a->sep_ext, &sep, sizeof (sep));
  if ((rv = application_connect (a)))
    return clib_error_return_code (0, rv, 0, "connect failed");
  return 0;
}

int
vnet_disconnect_session (vnet_disconnect_args_t * a)
{
  if (session_handle_is_local (a->handle))
    {
      local_session_t *ls;

      /* Disconnect reply came to worker 1 not main thread */
      app_interface_check_thread_and_barrier (vnet_disconnect_session, a);

      if (!(ls = app_worker_get_local_session_from_handle (a->handle)))
	return 0;

      return app_worker_local_session_disconnect (a->app_index, ls);
    }
  else
    {
      app_worker_t *app_wrk;
      session_t *s;

      s = session_get_from_handle_if_valid (a->handle);
      if (!s)
	return VNET_API_ERROR_INVALID_VALUE;
      app_wrk = app_worker_get (s->app_wrk_index);
      if (app_wrk->app_index != a->app_index)
	return VNET_API_ERROR_INVALID_VALUE;

      /* We're peeking into another's thread pool. Make sure */
      ASSERT (s->session_index == session_index_from_handle (a->handle));

      session_close (s);
    }
  return 0;
}

clib_error_t *
vnet_listen (vnet_listen_args_t * a)
{
  int rv;
  if ((rv = vnet_listen_inline (a)))
    return clib_error_return_code (0, rv, 0, "bind failed: %d", rv);
  return 0;
}

clib_error_t *
vnet_unlisten (vnet_unbind_args_t * a)
{
  int rv;
  if ((rv = vnet_unlisten_inline (a)))
    return clib_error_return_code (0, rv, 0, "unbind failed: %d", rv);
  return 0;
}

clib_error_t *
vnet_connect (vnet_connect_args_t * a)
{
  int rv;

  if ((rv = application_connect (a)))
    return clib_error_return_code (0, rv, 0, "connect failed: %d", rv);
  return 0;
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
