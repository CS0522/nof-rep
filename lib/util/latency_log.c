#include "spdk/util.h"
#ifdef LANTENCY_LOG
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
			FILE* file = fopen(LOGFILEPATH, "w+");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
		}else{
			FILE* file = fopen(LOGFILEPATH, "a");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
		}
	}else{
		log_num++;
		sprintf(log_temp_char, "%s%u,%s,%llu:%llu,%llu:%llu\n", log_temp_char, io_id, module, start_time.tv_sec, start_time.tv_nsec, end_time.tv_sec, end_time.tv_nsec);
		if(log_num == UPDATE_PERIOD){
			FILE* file = fopen(LOGFILEPATH, "w+");
			fprintf(file, "%s", log_temp_char);
			fclose(file);
			memset(log_temp_char, 0, MAX_CHAR_CACHE);
		}else if(log_num % UPDATE_PERIOD == 0){
			FILE* file = fopen(LOGFILEPATH, "a");
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