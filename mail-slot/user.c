#include <stdio.h>
#include <unistd.h>

long val;

int main(int argc, char** argv){
	
	printf("I'm process %d - target addr is: %lu\n",getpid(),&val);

	while(1){
		sleep(3);
		printf("value id: %ld\n",val);

	}

}
