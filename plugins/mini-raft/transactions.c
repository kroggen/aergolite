
SQLITE_PRIVATE void new_block_timer_cb(uv_timer_t* handle);

/****************************************************************************/

SQLITE_PRIVATE void on_local_transaction_sent(send_message_t *req, int status) {

  if (status < 0) {
    //plugin->sync_up_state = DB_STATE_UNKNOWN;
    sqlite3_log(status, "sending local transaction: %s\n", uv_strerror(status));
    uv_close2( (uv_handle_t*) ((uv_write_t*)req)->handle, worker_thread_on_close);  /* disconnect */
  }

}

/****************************************************************************/

SQLITE_PRIVATE int send_local_transaction_data(plugin *plugin, int64 nonce, binn *log) {
  aergolite *this_node = plugin->this_node;
  binn *map=NULL;
  BOOL ret;

  SYNCTRACE("send_local_transaction_data - nonce=%" INT64_FORMAT "\n", nonce);

  plugin->sync_up_state = DB_STATE_SYNCHRONIZING;

  map = binn_map();
  if( !map ) goto loc_failed;

  binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_INSERT_TRANSACTION);
  binn_map_set_int32(map, PLUGIN_NODE_ID, plugin->node_id);
  binn_map_set_int64(map, PLUGIN_NONCE, nonce);
  binn_map_set_list(map, PLUGIN_SQL_CMDS, log);

  ret = send_peer_message(plugin->leader_node, map, on_local_transaction_sent);
  if( ret==FALSE ) goto loc_failed;

  binn_free(map);

  return SQLITE_OK;

loc_failed:

  sqlite3_log(1, "send_local_transaction_data failed");

  plugin->sync_up_state = DB_STATE_LOCAL_CHANGES;

  if( map ) binn_free(map);

  return SQLITE_ERROR;

}

/****************************************************************************/

SQLITE_PRIVATE void send_local_transactions(plugin *plugin) {
  aergolite *this_node = plugin->this_node;
  int64 nonce;
  binn *log = NULL;
  int rc;

  SYNCTRACE("send_local_transactions\n");

  nonce = 0; /* we don't know what is the nonce of the first txn in the queue */

  while( 1 ){
    rc = aergolite_get_local_transaction(this_node, &nonce, &log);

    if( rc==SQLITE_EMPTY || rc==SQLITE_NOTFOUND ){
      SYNCTRACE("send_local_transactions - no more local transactions - IN SYNC\n");
      plugin->sync_up_state = DB_STATE_IN_SYNC;
      return;
    } else if( rc!=SQLITE_OK || nonce==0 || log==0 ){
      sqlite3_log(rc, "send_local_transactions FAILED - nonce=%" INT64_FORMAT, nonce);
      plugin->sync_up_state = DB_STATE_LOCAL_CHANGES;
      return;
    }

    if( !plugin->leader_node ){
      SYNCTRACE("send_local_transactions - no leader node\n");
      plugin->sync_up_state = DB_STATE_LOCAL_CHANGES;
      aergolite_free_transaction(log);
      return;
    }

    rc = send_local_transaction_data(plugin, nonce, log);
    aergolite_free_transaction(log);
    if( rc ) return;

    nonce++;
  }

}

/****************************************************************************/
/****************************************************************************/

#if 0

SQLITE_PRIVATE void on_transaction_exists(node *node, void *msg, int size) {
  plugin *plugin = node->plugin;
  aergolite *this_node = node->this_node;
  int64 tid;

  tid = binn_map_int64(msg, PLUGIN_TID);

  SYNCTRACE("on_transaction_exists - tid=%" INT64_FORMAT "\n", tid);

}

/****************************************************************************/

SQLITE_PRIVATE void on_transaction_failed(plugin *plugin, int64 tid, int rc) {
  aergolite *this_node = plugin->this_node;

  if( rc==SQLITE_BUSY ){  /* temporary failure */
    SYNCTRACE("on_transaction_failed - TEMPORARY FAILURE - tid=%" INT64_FORMAT "\n", tid);
    /* retry the command */
  } else {  /* definitive failure */
    struct transaction *txn;
    /* the command is invalid and it cannot be included in the list */
    SYNCTRACE("on_transaction_failed - DEFINITIVE FAILURE - tid=%" INT64_FORMAT " rc=%d\n", tid, rc);
    /* search for the transaction in the mempool */
    for( txn=plugin->mempool; txn; txn=txn->next ){
      if( txn->id==tid ) break;
    }
    if( txn ){
      /* remove the transaction from the mempool */
      discard_mempool_transaction(plugin, txn);
    }
  }

}

/****************************************************************************/

SQLITE_PRIVATE void on_transaction_failed_msg(node *node, void *msg, int size) {
  plugin *plugin = node->plugin;
  aergolite *this_node = node->this_node;
  int64 tid;
  int rc;

  tid = binn_map_int64(msg, PLUGIN_TID);
  rc = binn_map_int32(msg, PLUGIN_ERROR);

  on_transaction_failed(plugin, tid, rc);

}

#endif

/****************************************************************************/

/*
** Used by the leader.
*/
SQLITE_PRIVATE int broadcast_transaction(plugin *plugin, struct transaction *txn) {
  struct node *node;
  binn *map;

  SYNCTRACE("broadcast_transaction"
            " node=%d nonce=%" INT64_FORMAT " sql_count=%d\n",
            txn->node_id, txn->nonce, binn_count(txn->log)-2 );

  /* signal other peers that there is a new transaction */
  map = binn_map();
  if( !map ) return SQLITE_BUSY;  /* flag to retry the command later */

  binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_NEW_TRANSACTION);
  binn_map_set_int32(map, PLUGIN_NODE_ID, txn->node_id);
  binn_map_set_int64(map, PLUGIN_NONCE, txn->nonce);
  binn_map_set_list (map, PLUGIN_SQL_CMDS, txn->log);

  for( node=plugin->peers; node; node=node->next ){
//    if( node->id!=txn->node_id ){   -- should it notify the same node?
      send_peer_message(node, map, NULL);
//    }
  }

  binn_free(map);

  return SQLITE_OK;
}

/****************************************************************************/

SQLITE_PRIVATE struct transaction * store_transaction_on_mempool(
  plugin *plugin, int node_id, int64 nonce, void *log
){
  struct transaction *txn;

  SYNCTRACE("store_transaction_on_mempool node_id=%d nonce=%"
            INT64_FORMAT "\n", node_id, nonce);

  txn = sqlite3_malloc_zero(sizeof(struct transaction));
  if( !txn ) return NULL;

  llist_add(&plugin->mempool, txn);

  /* transaction data */
  txn->node_id = node_id;
  txn->nonce = nonce;
  txn->id = aergolite_get_transaction_id(node_id, nonce);
  txn->log = sqlite3_memdup(binn_ptr(log), binn_size(log));
  //! it could create a copy here using only the SQL commands and removing not needed data. or maybe use netstring...
  //txn->data = xxx(log);    //! or maybe let the consensus protocol decide what to store here...
  //! or the core could supply the txn already without the metadata

  return txn;
}

/****************************************************************************/

SQLITE_PRIVATE void discard_mempool_transaction(plugin *plugin, struct transaction *txn){

  SYNCTRACE("discard_mempool_transaction\n");

  /* remove the transaction from the mempool */
  if( txn ){
    llist_remove(&plugin->mempool, txn);
    if( txn->log ) sqlite3_free(txn->log);
    sqlite3_free(txn);
  }

}

/****************************************************************************/

/*
** Used by the leader.
** -verify the transaction  ??
** -store the transaction in the local mempool,
** -if the timer to generate a new block is not started, start it now, and
** -broadcast the transaction to all the peers.
*/
SQLITE_PRIVATE int process_new_transaction(plugin *plugin, int node_id, int64 nonce, void *log) {
  struct transaction *txn;
  int64 tid;
  int rc;

  SYNCTRACE("process_new_transaction - node=%d nonce=%" INT64_FORMAT " sql_count=%d\n",
            node_id, nonce, binn_count(log)-2 );

  tid = aergolite_get_transaction_id(node_id, nonce);

  /* check if the transaction is already in the local mempool */
  for( txn=plugin->mempool; txn; txn=txn->next ){
    if( txn->id==tid ){
      SYNCTRACE("process_new_transaction - transaction already on mempool\n");
      return SQLITE_EXISTS;
    }
  }
  if( !txn ){
    /* store the transaction in the local mempool */
    txn = store_transaction_on_mempool(plugin, node_id, nonce, log);
    if( !txn ) return SQLITE_NOMEM;
  }

  /* start the timer to generate a new block */
  if( !uv_is_active((uv_handle_t*)&plugin->new_block_timer) ){
    uv_timer_start(&plugin->new_block_timer, new_block_timer_cb, NEW_BLOCK_WAIT_INTERVAL, 0);
  }

  /* broadcast the transaction to all the peers */
  rc = broadcast_transaction(plugin, txn);

  return rc;
}

/****************************************************************************/

/*
** The leader node received a new transaction from a follower node
*/
SQLITE_PRIVATE void on_insert_transaction(node *source_node, void *msg, int size) {
  plugin *plugin = source_node->plugin;
  aergolite *this_node = source_node->this_node;
  int64 nonce;
  BOOL tr_exists;
  void *log=0;
  int  rc;

  nonce = binn_map_int64(msg, PLUGIN_NONCE);
  log = binn_map_list(msg, PLUGIN_SQL_CMDS);

  SYNCTRACE("on_insert_transaction - from node %d - nonce: %" INT64_FORMAT
            " sql count: %d\n", source_node->id, nonce, binn_count(log)-2 );


#if 0

  //! instead, it must call a fn to check the nonce for this node


  if( aergolite_check_transaction_in_blockchain(this_node,tid,&tr_exists)!=SQLITE_OK ){
    sqlite3_log(1, "check_transaction_in_blockchain failed");
    rc = SQLITE_BUSY;  /* to retry again */
    goto loc_failed;
  }

  if( tr_exists ){
    binn *map = binn_map();
    if (!map) return;
    binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_LOG_EXISTS);
    binn_map_set_int64(map, PLUGIN_NONCE, nonce);
    send_peer_message(source_node, map, NULL);
    binn_free(map);
    return;
  }

#endif


  rc = process_new_transaction(plugin, source_node->id, nonce, log);
  if( rc ) goto loc_failed;

  return;

loc_failed:

  if( rc==SQLITE_OK ) rc = SQLITE_BUSY;  /* to retry again */
  {
    binn *map = binn_map();
    if (!map) return;
    binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_TRANSACTION_FAILED);
    binn_map_set_int64(map, PLUGIN_NONCE, nonce);
    binn_map_set_int32(map, PLUGIN_ERROR, rc);
    send_peer_message(source_node, map, NULL);
    binn_free(map);
  }

}

/****************************************************************************/

/*
** A new transaction was received from the leader node
*/
SQLITE_PRIVATE int on_new_remote_transaction(node *node, void *msg, int size) {
  plugin *plugin = node->plugin;
  struct transaction *txn;
  int node_id;
  int64 nonce;
  void *log;

  node_id = binn_map_int32(msg, PLUGIN_NODE_ID);
  nonce   = binn_map_int64(msg, PLUGIN_NONCE);
  log     = binn_map_list (msg, PLUGIN_SQL_CMDS);

  SYNCTRACE("on_new_remote_transaction - node_id=%d nonce=%" INT64_FORMAT
            " sql_count=%d\n", node_id, nonce, binn_count(log)-2 );

  /* store the transaction in the local mempool */
  txn = store_transaction_on_mempool(plugin, node_id, nonce, log);
  if( !txn ) return SQLITE_NOMEM;

  return SQLITE_OK;
}

/****************************************************************************/

SQLITE_PRIVATE void leader_node_process_local_transactions(plugin *plugin) {
  aergolite *this_node = plugin->this_node;
  int64 nonce;
  binn *log=NULL;
  int   rc;

  SYNCTRACE("leader_node_process_local_transactions\n");

  nonce = 0;

  while( 1 ){
    rc = aergolite_get_local_transaction(this_node, &nonce, &log);

    if( rc==SQLITE_EMPTY || rc==SQLITE_NOTFOUND ){
      SYNCTRACE("leader_node_process_local_transactions - no more local transactions - IN SYNC\n");
      plugin->sync_up_state = DB_STATE_IN_SYNC;
      return;
    } else if( rc!=SQLITE_OK || nonce==0 || log==0 ){
      SYNCTRACE("--- leader_node_process_local_transactions FAILED - rc=%d nonce=%" INT64_FORMAT " log=%p\n", rc, nonce, log);
      plugin->sync_up_state = DB_STATE_UNKNOWN;
      goto loc_try_later;
    }

    // it must enter in a consensus with other nodes before executing a transaction.
    // this process is asynchronous, it must receive notifications from the other nodes.
    // it counts the number of received messages for the sent transaction.
    // then waits until it receives response messages from the majority of the peers (including itself)

    // so the nodes must store the total number of nodes (or the list of nodes) and they must
    // agree on this list - it can be on the blockchain!

    rc = process_new_transaction(plugin, plugin->node_id, nonce, log);
    aergolite_free_transaction(log);
    if( rc ) goto loc_try_later;

    nonce++;
  }


loc_try_later:

  plugin->sync_up_state = DB_STATE_LOCAL_CHANGES;

  /* activate a timer to retry it again later */
//  SYNCTRACE("starting the process local transactions timer\n");
//  uv_timer_start(&plugin->process_transactions_timer, process_transactions_timer_cb, 100, 0);

}

/****************************************************************************/

SQLITE_PRIVATE void follower_node_on_local_transaction(plugin *plugin) {

  SYNCTRACE("follower_node_on_local_transaction\n");

  /* if this node is in sync, just send the new local transaction. otherwise start the sync process */

  if( plugin->sync_up_state!=DB_STATE_SYNCHRONIZING ){
    /* update the upstream state */
    plugin->sync_up_state = DB_STATE_LOCAL_CHANGES;
    /* check if already downloaded all txns */
    if( plugin->sync_down_state==DB_STATE_IN_SYNC ){
      /* send the new local transaction */
      send_local_transactions(plugin);
    }
  }

}

/****************************************************************************/

SQLITE_PRIVATE void leader_node_on_local_transaction(plugin *plugin) {

  SYNCTRACE("leader_node_on_local_transaction\n");

  // if this is a primary node, it must:
  // -save the command on the -wal-remote, using the log table
  // -send the 'new command' notification to the connected [follower] nodes
  // -start a log rotation

  plugin->sync_up_state = DB_STATE_LOCAL_CHANGES;

  leader_node_process_local_transactions(plugin);

}

/****************************************************************************/

SQLITE_PRIVATE void db_sync_on_local_transaction(plugin *plugin) {

  SYNCTRACE("db_sync_on_local_transaction\n");

  if( plugin->is_leader ){
    leader_node_on_local_transaction(plugin);
  }else{
    follower_node_on_local_transaction(plugin);
  }

}

/****************************************************************************/

SQLITE_PRIVATE void worker_thread_on_local_transaction(plugin *plugin) {

  SYNCTRACE("worker thread: on new local transaction\n");

  /* start the db sync if not yet started */
  db_sync_on_local_transaction(plugin);

}

/****************************************************************************/

/*
** This function is called on the main thread. It must send the notification
** to the worker thread and return as fast as possible.
*/
SQLITE_API void on_new_local_transaction(void *arg) {
  plugin *plugin = (struct plugin *) arg;

  SYNCTRACE("on_new_local_transaction\n");

  if( plugin->thread_active ){
    int rc, cmd = WORKER_THREAD_NEW_TRANSACTION;
    /* send command to the worker thread */
    SYNCTRACE("sending worker thread command: new local transaction\n");
    if( (rc=send_notification_to_worker(plugin, (char*)&cmd, sizeof(cmd))) < 0 ){
      SYNCTRACE("send_notification_to_worker failed: (%d) %s\n", rc, uv_strerror(rc));
    }
  }

}
