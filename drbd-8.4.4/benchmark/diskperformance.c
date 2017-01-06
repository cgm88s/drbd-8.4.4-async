
//disk io performace test.  ioperfor.c
// this file from DRBD, ��Դ�빦����IO���ܲ���.������drbd/benchmark
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#define min(a,b) ( (a) < (b) ? (a) : (b) )

unsigned long long fsize(int in_fd){   //��ȡ�ļ���С
	struct stat dm_stat;
	unsigned long long size;
	if (fstat(in_fd, &dm_stat)) {
		fprintf(stderr, "Can not fstat\n");
		exit(20);
	}
	if (S_ISBLK(dm_stat.st_mode)) {
		unsigned long ls;
		if (ioctl(in_fd, BLKGETSIZE, &ls)) {
			fprintf(stderr, "Can not ioctl(BLKGETSIZE)\n");
			exit(20);
		}
		size = ((unsigned long long)ls) * 512;
	} else if (S_ISREG(dm_stat.st_mode)) {
		size = dm_stat.st_size;
	} else{
		size = -1;
	}
	return size;
}

unsigned long long m_strtol(const char *s){
	char *e = (char *)s;
	unsigned long long r;
	r = strtol(s, &e, 0);
	switch (*e) {
		case 0:
			return r;
		case 's':
		case 'S':
			return r << 9; // * 512;
		case 'K':
		case 'k':
			return r << 10; // * 1024;
		case 'M':
		case 'm':
			return r << 20; // * 1024 * 1024;
		case 'G':
		case 'g':
			return r << 30; // * 1024 * 1024 * 1024;
		default:
			fprintf(stderr, "%s is not a valid number\n", s);
			exit(20);
	}
}

void usage(char *prgname)
{
	fprintf(stderr, "USAGE: %s [options] \n"
		"  Available options:\n"
		"   --input-pattern val -a val \n"
		"   --input-file val    -i val \n"
		"   --output-file val   -o val\n"
		"   --connect-port val  -P val\n"
		"   --connect-ip val    -c val\n"
		"     instead of -o you might use -P and -c\n"
		"   --buffer-size val   -b val\n"
		"   --seek-input val    -k val\n"
		"   --seek-output val   -l val\n"
		"   --size val          -s val\n"
		"   --o_direct          -x\n"
		"     affect -i and -o \n"
		"   --bandwidth         -w val byte/second \n"
		"   --sync              -y\n"
		"   --progress          -m\n"
		"   --performance       -p\n"
		"   --dialog            -d\n"
		"   --help              -h\n", prgname);
	exit(20);

}

int connect_to (char *ip, int port)
{
	int fd;
	struct sockaddr_in addr;
	int ret;

	if((fd = socket (PF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		exit(20);
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons (port);
	if (inet_aton (ip, &addr.sin_addr) == 0) {
		fprintf(stderr, "Error in inet_aton (%s).\n", ip);
		close (fd);
		exit(20);
	}
	if ((ret = connect (fd, (struct sockaddr*) &addr, sizeof (addr))) < 0) {
		perror("connect");
		close (fd);
		exit(20);
	}
	return fd;
}


int main(int argc, char **argv)
{
	void *buffer;
	size_t rr, ww;
	unsigned long long seek_offs_i = 0;
	unsigned long long seek_offs_o = 0;
	unsigned long long size = -1, rsize;
	int in_fd = 0, out_fd = 1;
	unsigned long buffer_size = 65536;
	unsigned long long target_bw = 0;
	int o_direct = 0;
	int do_sync = 0;
	int show_progress = 0;
	int show_performance = 0;
	struct timeval tv1, tv2;
	int use_pattern = 0;
	int pattern;
	int dialog = 0, show_input_size = 0;
	int last_percentage = 0;

	char *input_file_name = NULL;
	char *output_file_name = NULL;
	char *connect_target = NULL;
	int connect_port = 0;

	int c;
	static struct option options[] = {
		{"input-pattern", 1, 0, 'a'},	  //д���ݣ�
		{"input-file", 1, 0, 'i'},			//  ���ļ�
		{"output-file", 1, 0, 'o'},		//  д�ļ�
		{"connect-ip", 1, 0, 'c'},		//  д�����ַ
		{"connect-port", 1, 0, 'P'},	//  д����˿�
		{"buffer-size", 1, 0, 'b'},		//  ÿ��д�Ŀ��С
		{"seek-input", 1, 0, 'k'},		//	 ���ļ�ƫ��
		{"seek-output", 1, 0, 'l'},		//   д�ļ�ƫ��
		{"size", 1, 0, 's'},					//  �ܹ�д��С
		{"o_direct", 0, 0, 'x'},			//   DIRECTд
		{"bandwidth", 1, 0, 'w'},		//	   ����д����
		{"sync", 0, 0, 'y'},					//  	ͬ��д��־
		{"progress", 0, 0, 'm'},		//   ��ʾ��������
		{"performance", 0, 0, 'p'},	//  ��ʾ��������
		{"dialog", 0, 0, 'd'},				//  ��ʾд����
		{"help", 0, 0, 'h'},					//
		{"show-input-size", 0, 0, 'z'},	// //��ʾ�����ļ��Ĵ�С
		{0, 0, 0, 0}
	};

	if (argc == 1)
		usage(argv[0]);

	while (1) {
		c = getopt_long(argc, argv, "i:o:c:P:b:k:l:s:w:xympha:dz", options, 0);
		if (c == -1)
			break;
		switch (c) {
			case 'i':
				input_file_name = optarg; //�����ļ�
				break;
			case 'o':
				output_file_name = optarg; //����ļ�,д�ļ�
				break;
			case 'c':
				connect_target = optarg;  //���IP��ַ��д����
				break;
			case 'P':
				connect_port = m_strtol(optarg); //����˿�
				break;
			case 'b':
				buffer_size = m_strtol(optarg); //���С
				break;
			case 'k':
				seek_offs_i = m_strtol(optarg); //����ƫ��
				break;
			case 'l':
				seek_offs_o = m_strtol(optarg); //���ƫ��
				break;
			case 's':
				size = m_strtol(optarg); //�ܴ�С
				break;
			case 'x':
				o_direct = 1;   //O_DIRECT
				break;
			case 'y':
				do_sync = 1;  	//ͬ��д
				break;
			case 'm':
				show_progress = 1; //��ʾ������
				break;
			case 'p':
				show_performance = 1; //��ʾ��������
				break;
			case 'h':
				usage(argv[0]); //ʹ�ð���
				break;
			case 'a':
				use_pattern = 1;
				pattern = m_strtol(optarg);  //д��ֵ��1��1s,1k,1m,1g
				break;
			case 'd':
				dialog = 1; //��ʾд��ɱ���
				break;
			case 'z':
				show_input_size = 1;  //��ʾ�����ļ��Ĵ�С
				break;
			case 'w':
				target_bw = m_strtol(optarg); //����Ŀ���������
				break;
		}
	}

	if( output_file_name && connect_target ) { //д�ļ���д���粻��ͬʱ
		fprintf(stderr,"Both connect target and an output file name given.\n That is too much.\n");
		exit(20);
	}
	if(input_file_name) { //�򿪶��ļ�
		in_fd = open(input_file_name, O_RDONLY | (o_direct ? O_DIRECT : 0));
		/* if EINVAL, and o_direct, try again without it! */
		if (in_fd == -1 && errno == EINVAL && o_direct) {
			in_fd = open(input_file_name, O_RDONLY);
			if (in_fd >= 0)
				fprintf(stderr, "NOT using O_DIRECT for input file %s\n", input_file_name);
		}
		if (in_fd == -1) {
			fprintf(stderr,"Can not open input file/device\n");
			exit(20);
		}
	}
	if(output_file_name) { //��д�ļ�
		out_fd = open(output_file_name, O_WRONLY | O_CREAT | O_TRUNC |(o_direct? O_DIRECT : 0) , 0664);
		/* if EINVAL, and o_direct, try again without it! */
		if (out_fd == -1 && errno == EINVAL && o_direct) {
			out_fd = open(output_file_name, O_WRONLY | O_CREAT | O_TRUNC, 0664);
			if (out_fd >= 0)
				fprintf(stderr, "NOT using O_DIRECT for output file %s\n", output_file_name);
		}
		if (out_fd == -1) {
			fprintf(stderr,"Can not open output file/device\n");
			exit(20);
		}
	}

	if(connect_target) { //����д����
		out_fd = connect_to (connect_target, connect_port);
	}

	(void)posix_memalign(&buffer, sysconf(_SC_PAGESIZE), buffer_size); //�����СΪ���С�Ŀ黺������ҳ��С����
	if (!buffer) {
		fprintf(stderr, "Can not allocate the Buffer memory\n");
		exit(20);
	}

	if (seek_offs_i) { //����ƫ��
		if (lseek(in_fd, seek_offs_i, SEEK_SET) == -1) {
			fprintf(stderr,"Can not lseek(2) in input file/device\n");
			exit(20);
		}
	}

	if (seek_offs_o) { //�����ƫ��
		if (lseek(out_fd, seek_offs_o, SEEK_SET) == -1) {
			fprintf(stderr,"Can not lseek(2) in output file/device\n");
			exit(20);
		}
	}

	if (use_pattern) { //��ָ���������дbuffer
		memset(buffer, pattern, buffer_size);
	}

	if (dialog && size == -1) { //���û��ָ���ܴ�С�����ö�ȡ�ļ� ��д���ļ�����Сֵ
		size = min(fsize(in_fd), fsize(out_fd));
		if (size == -1) {
			fprintf(stderr, "Can not determine the size\n");
			exit(20);
		}
		if (size == 0) {
			fprintf(stderr, "Nothing to do?\n");
			exit(20);
		}
	}

	if (show_input_size) { //��ʾ�����ļ��Ĵ�С
		size = fsize(in_fd);
		if (size == -1) {
			fprintf(stderr, "Can not determine the size\n");
			exit(20);
		}
		printf("%lldK\n", size / 1024);
		exit(0);
	}

	rsize = size; //�ܴ�С
	gettimeofday(&tv1, NULL); //��ȡ��ǰʱ�䣬��ʼд��ʱ��
	while (1) {
		if (use_pattern)	//����û�ָ���� д���ݣ��Ͳ��ô��ļ����
			rr = min(buffer_size, rsize);
		else
			rr = read(in_fd, buffer,(size_t) min(buffer_size, rsize)); //�������ļ����� ��
		if (rr == 0) break;
		if (rr == -1) {
			if (errno == EINVAL && o_direct) {
				fprintf(stderr,"either leave off --o_direct,or fix the alignment of the buffer/size/offset!\n");
			}
			perror("Read failed");
			break;
		}
		if (show_progress) { //��ʾ������
			printf(rr == buffer_size ? "R" : "r");
			fflush(stdout);
		}
		ww = write(out_fd, buffer, rr); //������ļ�д�� д
		if (ww == -1) {
			if (errno == EINVAL && o_direct) {
				fprintf(stderr,"either leave off --o_direct, or fix the alignment of the buffer/size/offset!\n");
			}
			perror("Write failed");
			break;
		}
		rsize = rsize - ww; // ʣ�� ��С
		if (dialog) { //��ʾд��ɱ��� 0~100
			int new_percentage =(int)(100.0 * (size - rsize) / size);  //д��ɱ���
			if (new_percentage != last_percentage) { //ֻ�б�������ʱ����������ʾ
				printf("\r%3d", new_percentage);
				fflush(stdout);
				last_percentage = new_percentage; //��¼�ϴ���ɵı���
			}
		}
		if (target_bw) { //����Ŀ����������
			gettimeofday(&tv2, NULL);	//��ȡ��ǰʱ��
			long sec = tv2.tv_sec - tv1.tv_sec;
			long usec = tv2.tv_usec - tv1.tv_usec;		//�õ�ǰʱ�� - ��ʼдǰ��ʱ��= д����ʱ��
			double bps;
			double time_should;
			int time_wait;
			if (usec < 0) {
				sec--;
				usec += 1000000;
			}
			bps = ((double)(size - rsize)) /(sec + ((double)usec) / 1000000); //д������/д����ʱ��=��ǰд��������

			if ( bps > target_bw ) { //��ǰ����������������
				time_should = ((double)(size - rsize)) *1000 / target_bw; // mili seconds.
				time_wait = (int)(time_should -(sec*1000 + ((double)usec) / 1000));
				poll(NULL,0,time_wait); //��������
			}
		}
		if (ww != rr)  //���д��������дѭ��
			break;
	}
	//д���
	if (do_sync)		// ���������ͬ����־
		fsync(out_fd);  //��ˢ����ļ�

	gettimeofday(&tv2, NULL); 
	if (show_progress || dialog)
		printf("\n");

	if (show_performance) { //��ʾ��������
		long sec = tv2.tv_sec - tv1.tv_sec;
		long usec = tv2.tv_usec - tv1.tv_usec;
		double mps;
		if (usec < 0) {
			sec--;
			usec += 1000000;
		}
		mps = (((double)(size - rsize))/(1 << 20)) /(sec + ((double)usec) / 1000000);  //��������д����
		printf("%.2f MB/sec (%llu B / ", mps, size - rsize);
		printf("%02dh:%02dm:%02lds.%06ldus)\n", sec/3600,(sec%3600)/60, sec % 60, usec);
	}

	if (size != -1 && rsize) //�������ʣ���Сûд
		fprintf(stderr, "Could transfer only %lld Byte.\n",(size - rsize));

	return 0;
}
