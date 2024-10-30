#include "spdk/util.h"
#ifdef TARGET_LATENCY_LOG
#define MAX_CHAR_CACHE 100000
#define LOG_MAX_SIZE 100
#define UPDATE_PERIOD (MAX_CHAR_CACHE / LOG_MAX_SIZE)

char log_temp_char[MAX_CHAR_CACHE];

void write_log_to_file(uint32_t io_id, const char* module, struct timespec start_time, struct timespec end_time, bool is_finish){
	static uint64_t log_num = 0;
	if(log_num == 0){
		memset(log_temp_char, 0, MAX_CHAR_CACHE);
	}
	if(is_finish){
		if(log_num < UPDATE_PERIOD){
			FILE* file = fopen(TARGET_LOG_FILE_PATH, "w+");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
		}else{
			FILE* file = fopen(TARGET_LOG_FILE_PATH, "a");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
		}
	}else{
		log_num++;
		sprintf(log_temp_char, "%s%u,%s,%llu:%llu,%llu:%llu\n", log_temp_char, io_id, module, start_time.tv_sec, start_time.tv_nsec, end_time.tv_sec, end_time.tv_nsec);
		if(log_num == UPDATE_PERIOD){
			FILE* file = fopen(TARGET_LOG_FILE_PATH, "w+");
			fprintf(file, "io_id, modeule_name, start_time.tv_sec:start_time.nsec, end_time.tv_sec:end_time.tv_nsec\n");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
			memset(log_temp_char, 0, MAX_CHAR_CACHE);
		}else if(log_num % UPDATE_PERIOD == 0){
			FILE* file = fopen(TARGET_LOG_FILE_PATH, "a");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
			memset(log_temp_char, 0, MAX_CHAR_CACHE);
		}
	}
}

void write_latency_log(void* ctx){
	struct latency_log_ctx* latency_log = (struct latency_log_ctx*)ctx;
	write_log_to_file(latency_log->io_id, latency_log->module, latency_log->start_time, latency_log->end_time, false);
	free((struct latency_log_ctx*)ctx);
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
    sscanf(name, "RDMA (addr:%[0-9.] subnqn:%*[a-zA-Z0-9.:*-]) NSID %[0-9]", split_name, tmp);
    strcat(split_name, tmp);

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
