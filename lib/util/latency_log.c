#include "spdk/util.h"
#ifdef TARGET_LATENCY_LOG

pthread_mutex_t log_mutex;

struct latency_module_log module_log;

bool is_io_log = false;

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

int timespec_divide(struct timespec *ts, int num) {
    if (num <= 0) {
        // 无效的除数
        return -1;
    }

    // 先处理秒部分
    long sec_result = ts->tv_sec / num;
    long sec_remainder = ts->tv_sec % num;

    // 处理纳秒部分
    long nsec_result = ts->tv_nsec / num;
    long nsec_remainder = ts->tv_nsec % num;

    // 将剩余的秒部分（余数）转化为纳秒并加到纳秒部分
    long remainder_nsec_as_sec = sec_remainder * 1000000000L + nsec_remainder;
    nsec_result += remainder_nsec_as_sec / num;

    // 将可能的纳秒溢出部分加到秒
    sec_result += nsec_result / 1000000000L;
    nsec_result %= 1000000000L; // 保证纳秒部分小于1秒

    // 更新结果
    ts->tv_sec = sec_result;
    ts->tv_nsec = nsec_result;

    return 0; // 成功
}

void write_log_to_file(const char* module, struct timespec latency_time, uint32_t io_num){
    static uint64_t log_num = 0;
    if(!log_num){
	    FILE* file = fopen(TARGET_LOG_FILE_PATH, "w+");
        fprintf(file, "id, modeule_name, latency_time.sec:latency_time.nsec, io_num, average_latency.sec:average_latency.nsec\n");
        struct timespec temp = latency_time;
        timespec_divide(&temp, io_num);
	    fprintf(file, "%u,%s,%llu:%llu,%u,%llu:%llu\n", log_num / 3, module, latency_time.tv_sec, latency_time.tv_nsec, io_num, temp.tv_sec, temp.tv_nsec);
	    fclose(file);
    }else{
	    FILE* file = fopen(TARGET_LOG_FILE_PATH, "a");
        struct timespec temp = latency_time;
        timespec_divide(&temp, io_num);
	    fprintf(file, "%u,%s,%llu:%llu,%u,%llu:%llu\n", log_num / 3, module, latency_time.tv_sec, latency_time.tv_nsec, io_num, temp.tv_sec, temp.tv_nsec);
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

struct msg_buf
{
    long mtype;
    // msg 正文
    struct latency_log_task_ctx latency_log_tasks;
};

static int g_print_first_create_time_flag = 1;
static bool if_open = false;

/**
 * @name: write_log_tasks_to_file
 * @msg: write latency log of tasks to file
 * @param {*} fields of latency_log_task_ctx
 * @param {int} new_line: need to start a new line or not
 * @return {*}
 */
void write_log_tasks_to_file(uint32_t io_id, int ns_id,
                            struct timespec create_time, struct timespec submit_time,
                            struct timespec complete_time, 
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
        fprintf(file, "io_id:ns_id,create_time,submit_time,complete_time\n");
    }
    
    //char ch = fgetc(file);
    //printf("%c\n", ch);
    // 空文件则添加 title
    //if (ch == EOF)
    //{
    //    printf("File %s is empty, write the title line\n", HOST_LOG_FILE_PATH);
    //    fprintf(file, "io_id:ns_id,create_time,submit_time,complete_time\n");
    //}
    // 写入记录数据
    fprintf(file, "%u:%u,%llu:%llu,%llu:%llu,%llu:%llu", io_id, ns_id, 
                                    create_time.tv_sec, create_time.tv_nsec, 
                                    submit_time.tv_sec, submit_time.tv_nsec, 
                                    complete_time.tv_sec, complete_time.tv_nsec;
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
    //     printf("    log_task.ns_id = %u\n", latency_log_tasks[i].ns_id);
    //     printf("    log_task.create_time = %llu:%llu\n", latency_log_tasks[i].create_time.tv_sec, latency_log_tasks[i].create_time.tv_nsec);
    //     printf("    log_task.submit_time = %llu:%llu\n", latency_log_tasks[i].submit_time.tv_sec, latency_log_tasks[i].submit_time.tv_nsec);
    //     printf("    log_task.complete_time = %llu:%llu\n", latency_log_tasks[i].complete_time.tv_sec, latency_log_tasks[i].complete_time.tv_nsec);
    // }
    
    uint32_t rep_cnt = 0;
    for (; rep_cnt < g_rep_num; ++rep_cnt)
    {
        write_log_tasks_to_file(latency_log_tasks[rep_cnt].io_id, latency_log_tasks[rep_cnt].ns_id, 
                                latency_log_tasks[rep_cnt].create_time, latency_log_tasks[rep_cnt].submit_time,
                                latency_log_tasks[rep_cnt].complete_time,  
                                (g_rep_num != 1 && (rep_cnt == (g_rep_num - 1))) ? 1 : 0);
    }
    assert(rep_cnt == g_rep_num);

    //if (g_rep_num != 1)
        //g_print_first_create_time_flag = 0;

    // 是 msg 中的静态数组，不用 free
    // free((struct latency_log_task_ctx *)ctx);
}
#endif
