volatile int ha_tokudb_analyze_wait = 0; // debug

struct analyze_progress_extra {
    THD *thd;
    TOKUDB_SHARE *share;
    TABLE_SHARE *table_share;
    uint key_i;
    const char *key_name;
    time_t t_start;
    char *write_status_msg;
};

static int analyze_progress(void *v_extra, uint64_t rows) {
    struct analyze_progress_extra *extra = (struct analyze_progress_extra *) v_extra;
    THD *thd = extra->thd;
    if (thd->killed) 
        return ER_ABORTING_CONNECTION;

    time_t t_now = time(0);
    time_t t_limit = get_analyze_time(thd);
    time_t t_start = extra->t_start;
    if (t_limit > 0 && t_now - t_start > t_limit)
        return ETIME;
    float progress_rows = 0.0;
    TOKUDB_SHARE *share = extra->share;
    if (share->rows > 0)
        progress_rows = (float) rows / (float) share->rows;
    float progress_time = 0.0;
    if (t_limit > 0)
        progress_time = (float) (t_now - t_start) / (float) t_limit;
    char *write_status_msg = extra->write_status_msg;
    TABLE_SHARE *table_share = extra->table_share;
    sprintf(write_status_msg, "%s.%s.%s %u of %u %.lf%% rows %.lf%% time", 
            table_share->db.str, table_share->table_name.str, extra->key_name,
            extra->key_i, table_share->keys, progress_rows * 100.0, progress_time * 100.0);
    thd_proc_info(thd, write_status_msg);
    return 0;
}

int ha_tokudb::analyze(THD *thd, HA_CHECK_OPT *check_opt) {
    TOKUDB_DBUG_ENTER("ha_tokudb::analyze");
    while (ha_tokudb_analyze_wait) sleep(1); // debug concurrency issues
    uint64_t rec_per_key[table_share->key_parts];
    int result = HA_ADMIN_OK;
    DB_TXN *txn = transaction;
    if (!txn)
        result = HA_ADMIN_FAILED;
    if (result == HA_ADMIN_OK) {
        uint next_key_part = 0;
        // compute cardinality for each key
        for (uint i = 0; result == HA_ADMIN_OK && i < table_share->keys; i++) {
            KEY *key_info = &table_share->key_info[i];
            uint64_t num_key_parts = get_key_parts(key_info);
            const char *key_name = i == primary_key ? "primary" : key_info->name;
            struct analyze_progress_extra analyze_progress_extra = {
                thd, share, table_share, i, key_name, time(0), write_status_msg
            };
            int error = tokudb::analyze_card(share->key_file[i], txn, false, num_key_parts, &rec_per_key[next_key_part],
                                             tokudb_cmp_dbt_key_parts, analyze_progress, &analyze_progress_extra);
            if (error != 0 && error != ETIME) {
                result = HA_ADMIN_FAILED;
            } else {
                // debug
                if (tokudb_debug & TOKUDB_DEBUG_ANALYZE) {
                    fprintf(stderr, "ha_tokudb::analyze %s.%s.%s ", 
                            table_share->db.str, table_share->table_name.str, i == primary_key ? "primary" : table_share->key_info[i].name);
                    for (uint j = 0; j < num_key_parts; j++) 
                        fprintf(stderr, "%lu ", rec_per_key[next_key_part+j]);
                    fprintf(stderr, "\n");
                }
            }
            next_key_part += num_key_parts;
        } 
    }
    if (result == HA_ADMIN_OK)
        tokudb::set_card_in_status(share->status_block, txn, table_share->key_parts, rec_per_key);    
    TOKUDB_DBUG_RETURN(result);
}

static int hot_poll_fun(void *extra, float progress) {
    HOT_OPTIMIZE_CONTEXT context = (HOT_OPTIMIZE_CONTEXT)extra;
    if (context->thd->killed) {
        sprintf(context->write_status_msg, "The process has been killed, aborting hot optimize.");
        return ER_ABORTING_CONNECTION;
    }
    float percentage = progress * 100;
    sprintf(context->write_status_msg, "Optimization of index %u of %u about %.lf%% done", context->current_table + 1, context->num_tables, percentage);
    thd_proc_info(context->thd, context->write_status_msg);
#ifdef HA_TOKUDB_HAS_THD_PROGRESS
    if (context->progress_stage < context->current_table) {
        // the progress stage is behind the current table, so move up
        // to the next stage and set the progress stage to current.
        thd_progress_next_stage(context->thd);
        context->progress_stage = context->current_table;
    }
    // the percentage we report here is for the current stage/db
    thd_progress_report(context->thd, (unsigned long long) percentage, 100);
#endif
    return 0;
}

volatile int ha_tokudb_optimize_wait = 0; // debug

// flatten all DB's in this table, to do so, peform hot optimize on each db
int ha_tokudb::optimize(THD * thd, HA_CHECK_OPT * check_opt) {
    TOKUDB_DBUG_ENTER("ha_tokudb::optimize");
    while (ha_tokudb_optimize_wait) sleep(1); // debug

    int error;
    uint curr_num_DBs = table->s->keys + test(hidden_primary_key);

#ifdef HA_TOKUDB_HAS_THD_PROGRESS
    // each DB is its own stage. as HOT goes through each db, we'll
    // move on to the next stage.
    thd_progress_init(thd, curr_num_DBs);
#endif

    //
    // for each DB, run optimize and hot_optimize
    //
    for (uint i = 0; i < curr_num_DBs; i++) {
        DB* db = share->key_file[i];
        error = db->optimize(db);
        if (error) {
            goto cleanup;
        }
        struct hot_optimize_context hc;
        memset(&hc, 0, sizeof hc);
        hc.thd = thd;
        hc.write_status_msg = this->write_status_msg;
        hc.ha = this;
        hc.current_table = i;
        hc.num_tables = curr_num_DBs;
        error = db->hot_optimize(db, hot_poll_fun, &hc);
        if (error) {
            goto cleanup;
        }
    }

    error = 0;
cleanup:

#ifdef HA_TOKUDB_HAS_THD_PROGRESS
    thd_progress_end(thd);
#endif

    TOKUDB_DBUG_RETURN(error);
}

struct check_context {
    THD *thd;
};

static int ha_tokudb_check_progress(void *extra, float progress) {
    struct check_context *context = (struct check_context *) extra;
    int result = 0;
    if (context->thd->killed)
        result = ER_ABORTING_CONNECTION;
    return result;
}

static void ha_tokudb_check_info(THD *thd, TABLE *table, const char *msg) {
    if (thd->vio_ok()) {
        char tablename[256];
        snprintf(tablename, sizeof tablename, "%s.%s", table->s->db.str, table->s->table_name.str);
        thd->protocol->prepare_for_resend();
        thd->protocol->store(tablename, strlen(tablename), system_charset_info);
        thd->protocol->store("check", 5, system_charset_info);
        thd->protocol->store("info", 4, system_charset_info);
        thd->protocol->store(msg, strlen(msg), system_charset_info);
        thd->protocol->write();
    }
}

volatile int ha_tokudb_check_wait = 0; // debug

int ha_tokudb::check(THD *thd, HA_CHECK_OPT *check_opt) {
    TOKUDB_DBUG_ENTER("check");
    while (ha_tokudb_check_wait) sleep(1); // debug

    const char *old_proc_info = thd->proc_info;
    thd_proc_info(thd, "tokudb::check");

    int result = HA_ADMIN_OK;
    int r;

    int keep_going = 1;
    if (check_opt->flags & T_QUICK) {
        keep_going = 0;
    }
    if (check_opt->flags & T_EXTEND) {
        keep_going = 1;
    }

    r = acquire_table_lock(transaction, lock_write);
    if (r != 0)
        result = HA_ADMIN_INTERNAL_ERROR;
    if (result == HA_ADMIN_OK) {
        uint32_t num_DBs = table_share->keys + test(hidden_primary_key);
        snprintf(write_status_msg, sizeof write_status_msg, "%s primary=%d num=%d", share->table_name, primary_key, num_DBs);
        if (tokudb_debug & TOKUDB_DEBUG_CHECK) {
            ha_tokudb_check_info(thd, table, write_status_msg);
            time_t now = time(0);
            char timebuf[32];
            fprintf(stderr, "%.24s ha_tokudb::check %s\n", ctime_r(&now, timebuf), write_status_msg);
        }
        for (uint i = 0; i < num_DBs; i++) {
            DB *db = share->key_file[i];
            const char *kname = i == primary_key ? "primary" : table_share->key_info[i].name;
            snprintf(write_status_msg, sizeof write_status_msg, "%s key=%s %u", share->table_name, kname, i);
            thd_proc_info(thd, write_status_msg);
            if (tokudb_debug & TOKUDB_DEBUG_CHECK) {
                ha_tokudb_check_info(thd, table, write_status_msg);
                time_t now = time(0);
                char timebuf[32];
                fprintf(stderr, "%.24s ha_tokudb::check %s\n", ctime_r(&now, timebuf), write_status_msg);
            }
            struct check_context check_context = { thd };
            r = db->verify_with_progress(db, ha_tokudb_check_progress, &check_context, (tokudb_debug & TOKUDB_DEBUG_CHECK) != 0, keep_going);
            snprintf(write_status_msg, sizeof write_status_msg, "%s key=%s %u result=%d", share->table_name, kname, i, r);
            thd_proc_info(thd, write_status_msg);
            if (tokudb_debug & TOKUDB_DEBUG_CHECK) {
                ha_tokudb_check_info(thd, table, write_status_msg);
                time_t now = time(0);
                char timebuf[32];
                fprintf(stderr, "%.24s ha_tokudb::check %s\n", ctime_r(&now, timebuf), write_status_msg);
            }
            if (result == HA_ADMIN_OK && r != 0) {
                result = HA_ADMIN_CORRUPT;
                if (!keep_going)
                    break;
            }
        }
    }
    thd_proc_info(thd, old_proc_info);
    TOKUDB_DBUG_RETURN(result);
}
