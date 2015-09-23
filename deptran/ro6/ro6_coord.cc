
#include "ro6_coord.h"

namespace rococo {

void RO6Coord::deptran_start(TxnChopper *ch) {
  // new txn id for every new and retry.
  RequestHeader header = gen_header(ch);

  int pi;

  std::vector<Value> *input = nullptr;
  int32_t server_id;
  int     res;
  int     output_size;

  while ((res =
            ch->next_piece(input, output_size, server_id, pi,
                           header.p_type)) == 0) {
    header.pid = next_pie_id();

    rrr::FutureAttr fuattr;

    // remember this a asynchronous call! variable funtional range is important!
    fuattr.callback = [ch, pi, this, header](Future * fu) {
      bool early_return = false;
      {
        //     Log::debug("try locking at start response, tid: %llx, pid: %llx",
        // header.tid, header.pid);
        ScopedLock(this->mtx_);

        ChopStartResponse  res;
        fu->get_reply() >> res;

        if (IS_MODE_RO6) {
          std::vector<i64> ro;
          Compressor::string_to_vector(res.read_only, &ro);

          this->ro_txn_.insert(ro.begin(), ro.end());
        }

        // where should I store this graph?
        Graph<TxnInfo>& gra = *(res.gra_m.gra);
        Log::debug("start response graph size: %d", (int)gra.size());
        verify(gra.size() > 0);
        ch->gra_.Aggregate(gra);

        Log::debug(
          "receive deptran start response, tid: %llx, pid: %llx, graph size: %d",
          header.tid,
          header.pid,
          gra.size());

        if (gra.size() > 1) ch->disable_early_return();

        ch->n_started_++;

        if (ch->start_callback(pi, res)) this->deptran_start(ch);
        else if (ch->n_started_ == ch->n_pieces_) {
          this->deptran_finish(ch);

          if (ch->do_early_return()) {
            early_return = true;
          }
        }
      }

      if (early_return) {
        ch->reply_.res_ = SUCCESS;
        TxnReply& txn_reply_buf = ch->get_reply();
        double    last_latency  = ch->last_attempt_latency();
        this->report(txn_reply_buf, last_latency
#ifdef TXN_STAT
                     , ch
#endif // ifdef TXN_STAT
                     );
        ch->callback_(txn_reply_buf);
      }
    };

    RococoProxy *proxy = vec_rpc_proxy_[server_id];
    Log::debug("send deptran start request, tid: %llx, pid: %llx",
               ch->txn_id_,
               header.pid);
        verify(input != nullptr);
    Future::safe_release(proxy->async_rcc_start_pie(header, *input, fuattr));
  }
}

/** caller should be thread safe */
void RO6Coord::deptran_finish(TxnChopper *ch) {
  Log::debug("deptran finish, %llx", ch->txn_id_);

  // commit or abort piece
  rrr::FutureAttr fuattr;
  fuattr.callback = [ch, this](Future * fu) {
    int e = fu->get_error_code();
        verify(e == 0);

    bool callback = false;
    {
      ScopedLock(this->mtx_);
      ch->n_finished_++;

      ChopFinishResponse res;

      Log::debug("receive finish response. tid: %llx", ch->txn_id_);

      fu->get_reply() >> res;

      if (ch->n_finished_ == ch->proxies_.size()) {
        ch->finish_callback(res);
        callback = true;
      }
    }

    if (callback) {
      // generate a reply and callback.
      Log::debug("deptran callback, %llx", ch->txn_id_);

      if (!ch->do_early_return()) {
        ch->reply_.res_ = SUCCESS;
        TxnReply& txn_reply_buf = ch->get_reply();
        double    last_latency  = ch->last_attempt_latency();
        this->report(txn_reply_buf, last_latency
#ifdef TXN_STAT
                     , ch
#endif // ifdef TXN_STAT
                     );
        ch->callback_(txn_reply_buf);
      }
      delete ch;
    }
  };

  Log::debug(
    "send deptran finish requests to %d servers, tid: %llx, graph size: %d",
    (int)ch->proxies_.size(),
    ch->txn_id_,
    ch->gra_.size());
  verify(ch->proxies_.size() == ch->gra_.FindV(
           ch->txn_id_)->data_->servers_.size());

  ChopFinishRequest req;
  req.txn_id = ch->txn_id_;
  req.gra    = ch->gra_;

  if (IS_MODE_RO6) {
    // merge the read_only list.
    std::vector<i64> ro;
    ro.insert(ro.begin(), ro_txn_.begin(), ro_txn_.end());
    Compressor::string_to_vector(req.read_only, &ro);
  }

  verify(ch->gra_.size() > 0);
  verify(req.gra.size() > 0);

  for (auto& rp : ch->proxies_) {
    RococoProxy *proxy = vec_rpc_proxy_[rp];
    Future::safe_release(proxy->async_rcc_finish_txn(req, fuattr));
  }
}

void RO6Coord::ro6_start_ro(TxnChopper *ch) {
  // new txn id for every new and retry.
  RequestHeader header = gen_header(ch);

  int pi;

  std::vector<Value> *input = nullptr;
  int32_t server_id;
  int     res;
  int     output_size;

  while ((res =
            ch->next_piece(input, output_size, server_id, pi,
                           header.p_type)) == 0) {
    header.pid = next_pie_id();

    rrr::FutureAttr fuattr;

    // remember this a asynchronous call! variable funtional range is important!
    fuattr.callback = [ch, pi, this, header](Future * fu) {
      {
        ScopedLock(this->mtx_);

        std::vector<Value> res;
        fu->get_reply() >> res;

        Log::debug("receive deptran RO start response, tid: %llx, pid: %llx, ",
                   header.tid,
                   header.pid);

        ch->n_started_++;

        if (ch->read_only_start_callback(pi, NULL, res)) {
          this->ro6_start_ro(ch);
        } else if (ch->n_started_ == ch->n_pieces_) {
          // job finish here.
          ch->reply_.res_ = SUCCESS;

          // generate a reply and callback.
          Log::debug("ro6 RO callback, %llx", ch->txn_id_);
          TxnReply& txn_reply_buf = ch->get_reply();
          double    last_latency  = ch->last_attempt_latency();
          this->report(txn_reply_buf, last_latency
#ifdef TXN_STAT
                       , ch
#endif // ifdef TXN_STAT
                       );
          ch->callback_(txn_reply_buf);
          delete ch;

          //                    ch->read_only_reset();
          //                    this->deptran_finish_ro(ch);
          // TODO add a finish request to free the data structure on server.
        }
      }
    };

    RococoProxy *proxy = vec_rpc_proxy_[server_id];
    Log::debug("send deptran RO start request, tid: %llx, pid: %llx",
               ch->txn_id_,
               header.pid);
    verify(input != nullptr);
    Future::safe_release(proxy->async_rcc_ro_start_pie(header, *input, fuattr));
  }
}

void RO6Coord::do_one(TxnRequest & req) {
  // pre-process
  ScopedLock(this->mtx_);
  TxnChopper *ch = TxnChopperFactory::gen_chopper(req, benchmark_);
  ch->txn_id_ = this->next_txn_id();

  Log::debug("do one request");

  if (ccsi_) ccsi_->txn_start_one(thread_id_, ch->txn_type_);

  verify(!batch_optimal_);

  if (recorder_) {
    std::string log_s;
    req.get_log(ch->txn_id_, log_s);
    std::function<void(void)> start_func = [this, ch]() {
      if (ch->is_read_only()) ro6_start_ro(ch);
      else {
        deptran_start(ch);
      }
    };
    recorder_->submit(log_s, start_func);
  } else {
    if (ch->is_read_only()) ro6_start_ro(ch);
    else {
        deptran_start(ch);
    }
  }
}
} // namespace rococo
