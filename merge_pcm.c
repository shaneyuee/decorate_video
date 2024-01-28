#include <stdlib.h>
#include <stdio.h>

int main()
{
	FILE *f1 = fopen("1.pcm", "r");
	FILE *f2 = fopen("2.pcm", "r");
	FILE *f3 = fopen("3.pcm", "w");
	if(f1==NULL || f2==NULL || f3==NULL)
	{
		fprintf(stderr, "failed to open 1.pcm, 2.pcm or 3.pcm\n");
		return -1;
	}

	short a1, a2, a3;
	int nr = 0;
	while(fread(&a1, 2, 1, f1)==1 && fread(&a2, 2, 1, f2)==1)
	{
		a3 = (((int)a1*50 + (int)a2*100)/150);
		fwrite(&a3, 2, 1, f3);
		nr += 2;
	}
	printf("written %d bytes to 3.pcm\n", nr);
	return 0;
	
}
