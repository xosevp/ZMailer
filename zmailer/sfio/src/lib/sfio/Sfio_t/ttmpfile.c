#define creat	______creat
#define open	______open

#include	"sftest.h"

#undef	creat
#undef	open

static int	Open = 0;

#if __STD_C
int
open(const char* file, int mode, ...)
#else
int
open(file, mode)
char*	file;
int	mode;
#endif
{
	Open = 1;
	return 1001;
}

#if __STD_C
int
creat(const char* file, mode_t mode)
#else
int
create(file, mode)
char*	file;
int	mode;
#endif
{
	Open = 1;
	return 1001;
}

main()
{
	Sfio_t*	f;

	if(!(f = sftmp(1025)) )
		terror("Can't open temp file\n");
	if(Open)
		terror("Attempt to create file detected1\n");
	if(sfputc(f,0) < 0)
		terror("Can't write to temp file\n");
	if(Open)
		terror("Attempt to create file detected2\n");
	if(sfclose(f) < 0)
		terror("Can't close temp file\n");
	if(Open)
		terror("Attempt to create file detected3\n");

	if(!(f = sftmp(8)) )
		terror("Can't open temp file2\n");
	if(Open)
		terror("Attempt to create file detected4\n");
	sfdisc(f,NIL(Sfdisc_t*));
	if(!Open)
		terror("Real file wasn't created\n");

	return 0;
}
