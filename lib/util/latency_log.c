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

static int g_print_first_create_time_flag = 1;
static bool if_open = false;

struct latency_log_msg latency_msg;

void fprint_log(FILE* file, int i, int num, char* name, struct timespec latency, uint32_t io_num){
    struct timespec average_latency = latency;
    timespec_divide(&average_latency, io_num);
    fprintf(file, "%d,%u,%s,%llu:%llu,%u,%llu:%llu\n", num / namespace_num, i, name, latency.tv_sec, latency.tv_nsec, io_num, average_latency.tv_sec, average_latency.tv_nsec);
}

/**
 * @name: write_log_tasks_to_file
 * @msg: write latency log of tasks to file
 * @param {*} fields of latency_log_task_ctx
 * @param {int} new_line: need to start a new line or not
 * @return {*}
 */

void write_log_tasks_to_file(int i, uint32_t task_queue_io_num, struct timespec task_queue_latency, uint32_t task_complete_io_num, struct timespec task_complete_latency,
							uint32_t req_send_io_num, struct timespec req_send_latency, uint32_t req_complete_io_num, struct timespec req_complete_latency,
							uint32_t wr_send_io_num, struct timespec wr_send_latency, uint32_t wr_complete_io_num, struct timespec wr_complete_latency,
							int new_line){
    static int num = 0;
    FILE* file;
    if(!if_open){
        file = fopen(HOST_LOG_FILE_PATH, "w+");
    }else{
        file = fopen(HOST_LOG_FILE_PATH, "a");
    }
    if(!file){
        fprintf(stderr, "Failed to open %s\n", HOST_LOG_FILE_PATH);
        goto err;
    }
    if(!if_open){
        if_open = true;
        printf("File %s is empry, write the title line\n", HOST_LOG_FILE_PATH);
        fprintf(file, "id,ns_id,name,latency.sec:latency.nsec,io_num,average_latency.sec:average_latency.nsec\n");
    }
    fprint_log(file, i, num, "task_queue", task_queue_latency, task_queue_io_num);
    fprint_log(file, i, num, "task_complete", task_complete_latency, task_complete_io_num);
    fprint_log(file, i, num, "req_send", req_send_latency, req_send_io_num);
    fprint_log(file, i, num, "req_complete", req_complete_latency, req_complete_io_num);
    fprint_log(file, i, num, "wr_send", wr_send_latency, wr_complete_io_num);
    fprint_log(file, i, num, "wr_complete", wr_complete_latency, wr_complete_io_num);
    if(new_line){
        fprintf(file, "\n");
    }
    num++;
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
    struct latency_ns_log* latency_log_namespaces = (struct latency_ns_log*)ctx;

    for(int i = 0; i < namespace_num; i++){
        write_log_tasks_to_file(i, latency_log_namespaces[i].task_queue_latency.io_num, latency_log_namespaces[i].task_queue_latency.latency_time,
                                latency_log_namespaces[i].task_complete_latency.io_num, latency_log_namespaces[i].task_complete_latency.latency_time,
                                latency_log_namespaces[i].req_send_latency.io_num, latency_log_namespaces[i].req_send_latency.latency_time,
                                latency_log_namespaces[i].req_complete_latency.io_num, latency_log_namespaces[i].req_complete_latency.latency_time,
                                latency_log_namespaces[i].wr_send_latency.io_num, latency_log_namespaces[i].wr_send_latency.latency_time,
                                latency_log_namespaces[i].wr_complete_latency.io_num, latency_log_namespaces[i].wr_complete_latency.latency_time,
                                (i == namespace_num - 1 ? 1 : 0));
    }
    free((struct latency_ns_log*)ctx);
}

/* 检查 msg queue 消息个数 */
int check_msg_qnum(int msgid)
{
    struct msqid_ds msg_info;
    int msg_cnt;

    if (msgctl(msgid, IPC_STAT, &msg_info) == -1)
    {
        fprintf(stderr, "Failed to get msg queue info\n");
        exit(EXIT_FAILURE);
    }
    msg_cnt = msg_info.msg_qnum;

    return msg_cnt;
}

pthread_mutex_t log_mutex;
uint32_t namespace_num;
int msgid;

void cleanup_log(){
    for(int i = 0; i < namespace_num; i++){
        latency_msg.latency_log_namespaces[i].task_complete_latency.io_num = latency_msg.latency_log_namespaces[i].task_queue_latency.io_num = 0;
        latency_msg.latency_log_namespaces[i].task_complete_latency.latency_time.tv_sec = latency_msg.latency_log_namespaces[i].task_complete_latency.latency_time.tv_nsec = 0;
        latency_msg.latency_log_namespaces[i].task_queue_latency.latency_time.tv_sec = latency_msg.latency_log_namespaces[i].task_queue_latency.latency_time.tv_nsec = 0;
        latency_msg.latency_log_namespaces[i].req_send_latency.io_num = latency_msg.latency_log_namespaces[i].req_complete_latency.io_num = 0;
        latency_msg.latency_log_namespaces[i].req_send_latency.latency_time.tv_sec = latency_msg.latency_log_namespaces[i].req_send_latency.latency_time.tv_nsec = 0;
        latency_msg.latency_log_namespaces[i].req_complete_latency.latency_time.tv_sec = latency_msg.latency_log_namespaces[i].req_complete_latency.latency_time.tv_nsec = 0;
        latency_msg.latency_log_namespaces[i].wr_send_latency.io_num = latency_msg.latency_log_namespaces[i].wr_complete_latency.io_num = 0;
        latency_msg.latency_log_namespaces[i].wr_send_latency.latency_time.tv_sec = latency_msg.latency_log_namespaces[i].wr_send_latency.latency_time.tv_nsec = 0;
        latency_msg.latency_log_namespaces[i].wr_complete_latency.latency_time.tv_sec = latency_msg.latency_log_namespaces[i].wr_complete_latency.latency_time.tv_nsec = 0;
    }
}

void copy_latency_ns_log(struct latency_ns_log* temp){
    for(int i = 0; i < namespace_num; i++){
        temp[i] = latency_msg.latency_log_namespaces[i];
    }
}

bool is_io_num_not_empty(){
    bool return_judge = false;
    for(int i = 0; i < namespace_num; i++){
        if(latency_msg.latency_log_namespaces[i].task_complete_latency.io_num != 0 || latency_msg.latency_log_namespaces[i].task_queue_latency.io_num != 0 ||
            latency_msg.latency_log_namespaces[i].req_send_latency.io_num != 0 || latency_msg.latency_log_namespaces[i].req_complete_latency.io_num != 0 ||
            latency_msg.latency_log_namespaces[i].wr_send_latency.io_num != 0 || latency_msg.latency_log_namespaces[i].wr_complete_latency.io_num != 0){
            return_judge = true;
            break;
        }
    }
    return return_judge;
}

void latency_log_1s(union sigval sv){
	pthread_mutex_lock(&log_mutex);
	if(is_io_num_not_empty()){
        struct latency_log_msg latency_msg;
        latency_msg.mtype = 1;
        struct latency_ns_log* temp = malloc(namespace_num * sizeof(struct latency_ns_log));
        latency_msg.latency_log_namespaces = temp;
        copy_latency_ns_log(temp);
        msgsnd(msgid, &latency_msg, sizeof(namespace_num * sizeof(struct latency_ns_log)), 0);
        cleanup_log();
	}
	pthread_mutex_unlock(&log_mutex); 
}

void init_log_fn(){
    pthread_mutex_init(&log_mutex, NULL);

    cleanup_log();

    timer_t timerid;
    struct sigevent sev;
    struct itimerspec its;

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = latency_log_1s;
    sev.sigev_notify_attributes = NULL;
    sev.sigev_value.sival_ptr = latency_msg.latency_log_namespaces;

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

#endif
