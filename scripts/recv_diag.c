// recv_diag.c — #211 wire test: Edge node subscribes a topic and prints every
// received message via the event callback. Pair with send_diag on another phone
// (same topic) to prove Edge lightpush->fleet->filter delivery end-to-end at the
// transport layer (no MLS/app involved). arg1=mode(Edge|Core) arg2=topic arg3=secs
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef void (*FFICallBack)(int, const char *, size_t, void *);
typedef void *(*create_fn)(const char *, FFICallBack, void *);
typedef int (*op_fn)(void *, FFICallBack, void *);
typedef int (*s_fn)(void *, FFICallBack, void *, const char *);
static char g_buf[65536]; static int g_ret; static volatile int g_done;
static void cb(int r,const char*m,size_t l,void*u){(void)u;g_ret=r;size_t n=(m&&l)?(l<sizeof(g_buf)-1?l:sizeof(g_buf)-1):0;if(n)memcpy(g_buf,m,n);g_buf[n]=0;g_done=1;}
// event callback: fires on message_received; just print the payload envelope.
static void evcb(int r,const char*m,size_t l,void*u){(void)u;(void)r;
  if(m&&l){ if(strstr(m,"message_received")||strstr(m,"channel_message")) printf("## RECV: %.*s\n",(int)(l<400?l:400),m);}
}
static int wait_cb(void){for(int i=0;i<200&&!g_done;i++)usleep(50000);return g_ret;}
int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IOLBF,0);
  const char*mode=argc>1?argv[1]:"Edge";
  const char*topic=argc>2?argv[2]:"/diag/1/wiretest/proto";
  int secs=argc>3?atoi(argv[3]):120;
  void*h=dlopen("./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){printf("dlopen: %s\n",dlerror());return 1;}
  create_fn create=(create_fn)dlsym(h,"logosdelivery_create_node");
  op_fn start=(op_fn)dlsym(h,"logosdelivery_start_node");
  s_fn sub=(s_fn)dlsym(h,"logosdelivery_subscribe");
  op_fn setcb=(op_fn)dlsym(h,"logosdelivery_set_event_callback");
  char cfg[256];
  snprintf(cfg,sizeof(cfg),"{\"logLevel\":\"ERROR\",\"mode\":\"%s\",\"preset\":\"logos.dev\",\"tcpPort\":60180,\"discv5UdpPort\":60180}",mode);
  printf("=== RECV mode=%s topic=%s ===\n",mode,topic);
  g_done=0;void*ctx=create(cfg,cb,NULL);if(!ctx){printf("create fail\n");return 2;}
  if(setcb) setcb(ctx,evcb,NULL);         // register BEFORE start so no loss window
  g_done=0;start(ctx,cb,NULL);printf("start ret=%d\n",wait_cb());
  g_done=0;sub(ctx,cb,NULL,topic);printf("subscribe ret=%d\n",wait_cb());
  printf("listening %ds...\n",secs);
  for(int t=0;t<secs;t++) sleep(1);
  printf("done\n");
  return 0;
}
