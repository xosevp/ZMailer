#include	"sftest.h"

main()
{
	Sfio_t	*f, *sf;
	char	*ss, *s;
	int	n, i;
	char	zero[SF_BUFSIZE*2];
	char	buf[SF_BUFSIZE], small[512];
	Sfoff_t	one, two;

	s = "123456789\n";
	n = strlen(s);
	if(!(f = sfopen((Sfio_t*)0, Kpv[0],"w")))
		terror("Opening file to write\n");
	for(i = 0; i < 1000; ++i)
		if(sfwrite(f,s,n) != n)
			terror("Writing data\n");

	if(!(f = sfopen(f, Kpv[0],"r")))
		terror("Opening file to read\n");

	if(sfseek(f,(Sfoff_t)128,0) != (Sfoff_t)128)
		terror("Bad seek to 128\n");
	if(sfseek(f,(Sfoff_t)0,1) != (Sfoff_t)128)
		terror("Bad seek(0,1) to 128\n");

	if(sfseek(f,(Sfoff_t)0,2) != (i*n))
		terror("Bad file length\n");
	if(sftell(f) != (i*n))
		terror("sftell\n");
	for(; i > 0; --i)
	{	sfseek(f,(Sfoff_t)(-i*n),2);
		if(!(ss = sfgetr(f,'\n',1)))
			terror("sfgetr\n");
		if(strncmp(ss,s,sfvalue(f)-1) != 0)
			terror("Expect=%s\n",s);
	}

	if(!(f = sfopen(f,Kpv[0],"w")) )
		terror("Open to write\n");
	for(n = sizeof(zero)-1; n >= 0; --n)
		zero[n] = 0;
	if(sfwrite(f,zero,sizeof(zero)) != sizeof(zero))
		terror("Writing data\n");
	one = sfseek(f,(Sfoff_t)0,2);
	two = (Sfoff_t)lseek(sffileno(f),0L,2);
	if(one != two)
		terror("seeking1\n");
	if(sfseek(f,(Sfoff_t)(-1),2) != (Sfoff_t)lseek(sffileno(f),-1L,2))
		terror("seeking2\n");

	if(!(f = sfopen(f,Kpv[0],"w")))
		terror("Open to write2\n");
	for(n = 0; n < sizeof(buf); n++)
		buf[n] = n;
	for(n = 0; n < 256; n++)
		if(sfwrite(f,buf,sizeof(buf)) != sizeof(buf))
			terror("Writing data 2\n");
	if(!(f = sfopen(f,Kpv[0],"r")))
		terror("Open to read2\n");
	if(sfgetc(f) != 0 && sfgetc(f) != 1)
		terror("Get first 2 bytes\n");

	if(sfseek(f,(Sfoff_t)(128*sizeof(buf)),0) != (Sfoff_t)128*sizeof(buf) )
		terror("Seeking \n");
	for(n = 0; n < 128; ++n)
		if(sfread(f,buf,sizeof(buf)) != sizeof(buf))
			terror("Reading data\n");

	if(!(f = sfopen(f,Kpv[0],"r")))
		terror("Open to read3\n");
	sfsetbuf(f,small,sizeof(small));
	if(sfread(f, buf, 10) != 10)
		terror("sfread failed\n");
	if(sftell(f) != (Sfoff_t)10)
		terror("sftell failed\n");
	if(sfseek(f, (Sfoff_t)10, SEEK_CUR|SF_PUBLIC) != (Sfoff_t)20)
		terror("sfseek failed\n");
	sfseek(f, (Sfoff_t)0, SEEK_SET);

	if(!(sf = sfnew((Sfio_t*)0, small, sizeof(small), sffileno(f), SF_READ)) )
		terror("sfnew failed\n");
	if(sfread(f, buf, 10) != 10)
		terror("sfread failed2\n");
	if(sftell(f) != 10)
		terror("sftell failed2\n");

	if(sfseek(sf, (Sfoff_t)4000, SEEK_SET) != (Sfoff_t)4000)
		terror("sfseek failed on sf\n");
	sfsync(sf);

	if(sfseek(f, (Sfoff_t)10, SEEK_CUR|SF_PUBLIC) != (Sfoff_t)4010)
		terror("sfseek public failed\n");

	rmkpv();
	return 0;
}
