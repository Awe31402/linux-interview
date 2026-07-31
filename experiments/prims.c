#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
static void *tf(void *a){ return NULL; }
int main(int c, char **v){
  if(!strcmp(v[1],"fork")){ if(fork()==0) _exit(0); wait(NULL); }
  else if(!strcmp(v[1],"vfork")){ if(vfork()==0) _exit(0); wait(NULL); }
  else if(!strcmp(v[1],"pthread")){ pthread_t t; pthread_create(&t,NULL,tf,NULL); pthread_join(t,NULL); }
  return 0; }
