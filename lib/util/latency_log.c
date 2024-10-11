#include "spdk/util.h"
#ifdef LANTENCY_LOG
void write_log_to_file(uint64_t io_id, const char* module, struct timespec start_time, struct timespec end_time){
	static uint64_t log_num = 0;
	if(log_num == 0){
		FILE* file = fopen(TARGET_LOG_FILE_PATH, "w+");
		fprintf(file, "%llu,%s,%llu:%llu,%llu:%llu\n", io_id, module, start_time.tv_sec, start_time.tv_nsec, end_time.tv_sec, end_time.tv_nsec);
		fclose(file);
	}else{
		FILE* file = fopen(TARGET_LOG_FILE_PATH, "a");
		fprintf(file, "%llu,%s,%llu:%llu,%llu:%llu\n", io_id, module, start_time.tv_sec, start_time.tv_nsec, end_time.tv_sec, end_time.tv_nsec);
		fclose(file);
	}
	log_num++;
}

void write_latency_log(void* ctx){
	struct latency_log_ctx* latency_log = (struct latency_log_ctx*)ctx;
	write_log_to_file(latency_log->io_id, latency_log->module, latency_log->start_time, latency_log->end_time);
	free((struct latency_log_ctx*)ctx);
}


/**
 * @name: write_log_tasks_to_file
 * @msg: write latency log of tasks to file
 * @param {*} fields of latency_log_task_ctx
 * @param {int} new_line: need to start a new line or not
 * @return {*}
 */
void write_log_tasks_to_file(uint32_t io_id, int ns_index, int is_main_task, 
                            struct timespec create_time, struct timespec submit_time,
                            struct timespec complete_time, struct timespec all_complete_time,
                            int new_line)
{
    FILE *file = fopen(HOST_LOG_FILE_PATH, "a+");
    // 打开失败
    if (!file)
    {
        fprintf(stderr, "Failed to open %s\n", HOST_LOG_FILE_PATH);
        // exit(EXIT_FAILURE);
        goto err;
    }
    char ch = fgetc(file);
    // 空文件则添加 title
    if (ch == EOF)
    {
        printf("File %s is empty, write the title line\n", HOST_LOG_FILE_PATH);
        fprintf(file, "io_id:ns_index,is_main_task,create_time,submit_time,complete_time,all_complete_time\n");
    }
    // 写入记录数据
    fprintf(file, "%u:%d,%d,%llu:%llu,%llu:%llu,%llu:%llu,%llu:%llu\n", io_id, ns_index, is_main_task,
                                    create_time.tv_sec, create_time.tv_nsec, 
                                    submit_time.tv_sec, submit_time.tv_nsec, 
                                    complete_time.tv_sec, complete_time.tv_nsec, 
                                    all_complete_time.tv_sec, all_complete_time.tv_nsec);
    // 如果该任务的 n 个副本打印结束，空一行或者做行标
    if (new_line)
        fprintf(file, "\n");

err:
    fclose(file);
}

int get_ns_index(char *name, char **g_ns_name, uint32_t g_rep_num)
{
    if (!name || !g_ns_name)
    {
        fprintf(stderr, "Invalid ns_name\n");
        exit(EXIT_FAILURE);
    }

    int index = 0;
    for (; index < g_rep_num; ++index)
    {
        if (!strcmp(name, g_ns_name[index]))
            return index;
    }
    // fail to get index
    if (index >= g_rep_num)
        exit(EXIT_FAILURE);
}

/**
 * @name: write_latency_tasks_log
 * @msg: mapping the ns name of log_task with ns index
 * @param {void*} ctx: struct latency_log_tasks_head*
 * @param {char**} g_ns_name: splitted ns name for mapping ns index
 * @param {uint32_t} g_rep_num: replica num = ns num
 * @return {*}
 */
void write_latency_tasks_log(void* ctx, char **g_ns_name, uint32_t g_rep_num)
{
    struct latency_log_tasks_head *latency_log_tasks = (struct latency_log_tasks_head *)ctx;
    
    struct latency_log_task_ctx *lltc_t = NULL;
    uint32_t rep_cnt = 0;
    TAILQ_FOREACH(lltc_t, latency_log_tasks, link)
    {
        ++rep_cnt;
        write_log_tasks_to_file(lltc_t->io_id, get_ns_index(lltc_t->ns_entry_name, g_ns_name, g_rep_num), 
                                lltc_t->is_main_task,
                                lltc_t->create_time, lltc_t->submit_time,
                                lltc_t->complete_time, lltc_t->all_complete_time, 
                                rep_cnt == g_rep_num ? 1 : 0);
    }
    assert(rep_cnt == g_rep_num);
}
#endif