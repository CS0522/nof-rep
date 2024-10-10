#include "spdk/util.h"
#ifdef LANTENCY_LOG
void write_log_to_file(uint64_t io_id, const char* module, struct timespec start_time, struct timespec end_time){
	static uint64_t log_num = 0;
	if(log_num == 0){
		FILE* file = fopen(LOGFILEPATH, "w+");
		fprintf(file, "%llu,%s,%llu:%llu,%llu:%llu\n", io_id, module, start_time.tv_sec, start_time.tv_nsec, end_time.tv_sec, end_time.tv_nsec);
		fclose(file);
	}else{
		FILE* file = fopen(LOGFILEPATH, "a");
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
#endif