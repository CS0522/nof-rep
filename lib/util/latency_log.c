#include "spdk/util.h"
#include "spdk/thread.h"
#ifdef TARGET_LATENCY_LOG

pthread_mutex_t log_mutex;

struct latency_module_log module_log;

void latency_log_1s(union sigval sv){
    pthread_mutex_lock(&log_mutex);
    struct latency_module_log temp = module_log;
    spdk_thread_send_msg(spdk_thread_get_app_thread(), write_latency_log, &temp);
    pthread_mutex_unlock(&log_mutex);
}

void init_log_fn(){
    pthread_mutex_init(&log_mutex, NULL);

    timer_t timerid;
    struct sigevent sev;
    struct itimerspec its;

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = latency_log_1s;
    sev.sigev_notify_attributes = NULL;
    sev.sigev_value.sival_ptr = &module_log;

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        perror("timer_create");
        return 1;
    }

    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 1;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        perror("timer_settime");
        return 1;
    }
}

void fini_log_fn(){
    pthread_mutex_destroy(&log_mutex);
}

int timespec_sub(struct timespec *result, const struct timespec *a, const struct timespec *b) {
    result->tv_sec = a->tv_sec - b->tv_sec;
    result->tv_nsec = a->tv_nsec - b->tv_nsec;

    // 如果纳秒部分小于零，需要借 1 秒
    if (result->tv_nsec < 0) {
        result->tv_sec -= 1;
        result->tv_nsec += 1000000000;  // 纳秒值变为正数
    }

    // 如果秒数小于零，返回负数差值
    if (result->tv_sec < 0) {
        return -1;  // 返回负值表示 a 小于 b
    }

    return 0;  // 返回 0 表示 a >= b
}

void timespec_add(struct timespec *result, const struct timespec *a, const struct timespec *b) {
    result->tv_sec = a->tv_sec + b->tv_sec;
    result->tv_nsec = a->tv_nsec + b->tv_nsec;

    // 如果纳秒溢出，调整秒数和纳秒
    if (result->tv_nsec >= 1000000000) {
        result->tv_sec += 1;
        result->tv_nsec -= 1000000000;
    }
}

void write_log_to_file(const char* module, struct timespec latency_time, uint32_t io_num){
    static uint64_t log_num = 0;
    if(!log_num){
	    FILE* file = fopen(TARGET_LOG_FILE_PATH, "w+");
        fprintf(file, "id, modeule_name, latency_time.sec:latency_time.nsec, io_num, average_latency\n");
	    fprintf(file, "%u,%s,%llu:%llu,%u\n", module, latency_time.tv_sec, latency_time.tv_nsec, io_num);
	    fclose(file);
    }else{
	    FILE* file = fopen(TARGET_LOG_FILE_PATH, "a");
	    fprintf(file, "%u,%s,%llu:%llu,%u\n", module, latency_time.tv_sec, latency_time.tv_nsec, io_num);
	    fclose(file);
    }
    log_num++;
}

void write_latency_log(void* ctx){
	struct latency_module_log* latency_log = (struct latency_module_log*)ctx;
	write_log_to_file("target", latency_log->target.latency_time, latency_log->target.io_num);
    write_log_to_file("bdev", latency_log->bdev.latency_time, latency_log->bdev.io_num);
    write_log_to_file("driver", latency_log->driver.latency_time, latency_log->driver.io_num);
	free((struct latency_module_log*)ctx);
}
#endif

#ifdef PERF_LATENCY_LOG
static int g_print_first_create_time_flag = 1;
static bool if_open = false;

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
                            struct timespec first_create_time, 
                            int new_line)
{
    // myprint
    // printf("进入 write_log_tasks_to_file\n");

    FILE *file;
    if(!if_open){
        file = fopen(HOST_LOG_FILE_PATH, "w+");
    }else{
        file = fopen(HOST_LOG_FILE_PATH, "a");
    }
    // 打开失败
    if (!file)
    {
        fprintf(stderr, "Failed to open %s\n", HOST_LOG_FILE_PATH);
        // exit(EXIT_FAILURE);
        goto err;
    }

    if(!if_open){
        if_open = true;
        printf("File %s is empty, write the title line\n", HOST_LOG_FILE_PATH);
        fprintf(file, "io_id:ns_index,is_main_task,create_time,submit_time,complete_time,all_complete_time,first_create_time\n");
    }
    
    //char ch = fgetc(file);
    //printf("%c\n", ch);
    // 空文件则添加 title
    //if (ch == EOF)
    //{
    //    printf("File %s is empty, write the title line\n", HOST_LOG_FILE_PATH);
    //    fprintf(file, "io_id:ns_index,is_main_task,create_time,submit_time,complete_time,all_complete_time,first_create_time\n");
    //}
    // 写入记录数据
    fprintf(file, "%u:%d,%d,%llu:%llu,%llu:%llu,%llu:%llu,%llu:%llu", io_id, ns_index, is_main_task,
                                    create_time.tv_sec, create_time.tv_nsec, 
                                    submit_time.tv_sec, submit_time.tv_nsec, 
                                    complete_time.tv_sec, complete_time.tv_nsec, 
                                    all_complete_time.tv_sec, all_complete_time.tv_nsec);
    if (g_print_first_create_time_flag)
        fprintf(file, ",%llu:%llu", first_create_time.tv_sec, first_create_time.tv_nsec);
    fprintf(file, "\n");
    // 如果该任务的 n 个副本打印结束，空一行或者做行标
    if (new_line)
        fprintf(file, "\n");

err:
    fclose(file);
}

int get_ns_index(char *name, char **g_ns_name, uint32_t g_ns_num)
{
    // myprint
    // printf("进入 get_ns_index\n");

    if (!name || !g_ns_name)
    {
        fprintf(stderr, "Invalid ns_name\n");
        exit(EXIT_FAILURE);
    }

    // myprint
    // for (int i = 0; i < g_rep_num; ++i)
    // {
    //     printf("%s\n", g_ns_name[i]);
    // }

    char split_name[1024];
    char tmp[10];
    if(!strncmp(name, "PCIE", 4)){
    	sscanf(name, "PCIE (%[0-9:.]) NSID %[0-9]", split_name, tmp);
    	strcat(split_name, tmp);
    }else{
    	sscanf(name, "RDMA (addr:%[0-9.] subnqn:%*[a-zA-Z0-9.:*-]) NSID %[0-9]", split_name, tmp);
    	strcat(split_name, tmp);
    }

    int index = 0;
    for (; index < g_ns_num; ++index)
    {
        if (!strcmp(split_name, g_ns_name[index]))
            return index;
    }
    // failed to get index
    if (index >= g_ns_num)
        exit(EXIT_FAILURE);
}

/**
 * @name: write_latency_tasks_log
 * @msg: mapping the ns name of log_task with ns index
 * @param {void*} ctx: struct latency_log_tasks_head*
 * @param {char**} g_ns_name: splitted ns name for mapping ns index
 * @param {uint32_t} g_rep_num: replica num
 * @param {uint32_t} g_ns_num: namespace num
 * @return {*}
 */
void write_latency_tasks_log(void* ctx, char **g_ns_name, uint32_t g_rep_num, uint32_t g_ns_num)
{
    // myprint
    // printf("进入 write_latency_tasks_log\n");

    struct latency_log_task_ctx *latency_log_tasks = (struct latency_log_task_ctx *)ctx;

    // myprint
    // printf("log_task to write: \n");
    // for (int i = 0; i < 3; ++i)
    // {
    //     printf("*** log_task.io_id = %u ***\n", latency_log_tasks[i].io_id);
    //     printf("    log_task.is_main_task = %d\n", latency_log_tasks[i].is_main_task);
    //     printf("    log_task.ns_entry_name = %s\n", latency_log_tasks[i].ns_entry_name);
    //     printf("    log_task.create_time = %llu:%llu\n", latency_log_tasks[i].create_time.tv_sec, latency_log_tasks[i].create_time.tv_nsec);
    //     printf("    log_task.submit_time = %llu:%llu\n", latency_log_tasks[i].submit_time.tv_sec, latency_log_tasks[i].submit_time.tv_nsec);
    //     printf("    log_task.complete_time = %llu:%llu\n", latency_log_tasks[i].complete_time.tv_sec, latency_log_tasks[i].complete_time.tv_nsec);
    //     printf("    log_task.all_complete_time = %llu:%llu\n\n", latency_log_tasks[i].all_complete_time.tv_sec, latency_log_tasks[i].all_complete_time.tv_nsec);
    // }
    
    uint32_t rep_cnt = 0;
    for (; rep_cnt < g_rep_num; ++rep_cnt)
    {
        write_log_tasks_to_file(latency_log_tasks[rep_cnt].io_id, get_ns_index(latency_log_tasks[rep_cnt].ns_entry_name, g_ns_name, g_ns_num), 
                                latency_log_tasks[rep_cnt].is_main_task,
                                latency_log_tasks[rep_cnt].create_time, latency_log_tasks[rep_cnt].submit_time,
                                latency_log_tasks[rep_cnt].complete_time, latency_log_tasks[rep_cnt].all_complete_time, 
                                latency_log_tasks[rep_cnt].first_create_time, 
                                (g_rep_num != 1 && (rep_cnt == (g_rep_num - 1))) ? 1 : 0);
    }
    assert(rep_cnt == g_rep_num);

    //if (g_rep_num != 1)
        //g_print_first_create_time_flag = 0;

    // 是 msg 中的静态数组，不用 free
    // free((struct latency_log_task_ctx *)ctx);
}
#endif
