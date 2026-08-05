// metrics_diag.c — dump a fresh node's full prometheus Metrics after the mesh
// collapse window, so we can see WHY we get pruned (RLN validation / gossipsub
// score / relay mounted). Prints the whole blob; grep host-side.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef void (*FFICallBack)(int, const char *, size_t, void *);
typedef void *(*create_fn)(const char *, FFICallBack, void *);
typedef int (*op_fn)(void *, FFICallBack, void *);
typedef int (*sub_fn)(void *, FFICallBack, void *, const char *);
typedef int (*info_fn)(void *, FFICallBack, void *, const char *);
static char g_buf[1 << 20]; static int g_ret; static volatile int g_done;
static void cb(int r, const char *m, size_t l, void *u){(void)u;g_ret=r;size_t n=(m&&l)?(l<sizeof(g_buf)-1?l:sizeof(g_buf)-1):0;if(n)memcpy(g_buf,m,n);g_buf[n]=0;g_done=1;}
static int wait_cb(void){for(int i=0;i<200&&!g_done;i++)usleep(50000);return g_ret;}
int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IOLBF,0);
  void*h=dlopen(argc>1?argv[1]:"./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){printf("dlopen: %s\n",dlerror());return 1;}
  create_fn create=(create_fn)dlsym(h,"logosdelivery_create_node");
  op_fn start=(op_fn)dlsym(h,"logosdelivery_start_node");
  sub_fn sub=(sub_fn)dlsym(h,"logosdelivery_subscribe");
  info_fn info=(info_fn)dlsym(h,"logosdelivery_get_node_info");
  const char*cfg="{\"logLevel\":\"ERROR\",\"mode\":\"Core\",\"preset\":\"logos.dev\",\"tcpPort\":60150,\"discv5UdpPort\":60150}";
  g_done=0;void*ctx=create(cfg,cb,NULL);if(!ctx){printf("create fail\n");return 2;}
  g_done=0;start(ctx,cb,NULL);wait_cb();
  if(sub){g_done=0;sub(ctx,cb,NULL,"/kym/1/d3795c1d4f3306850dd1f422a3c962d2/proto");wait_cb();}
  sleep(45); // let the mesh collapse first
  g_done=0;info(ctx,cb,NULL,"Metrics");wait_cb();
  // print only lines of interest
  char*p=g_buf,*nl;
  while((nl=strchr(p,'\n'))){
    *nl=0;
    if(p[0]!='#' && (strstr(p,"rln")||strstr(p,"gossipsub")||strstr(p,"mesh")||strstr(p,"score")||strstr(p,"prune")||strstr(p,"graft")||strstr(p,"validated")||strstr(p,"invalid")||strstr(p,"relay")||strstr(p,"valid_messages")))
      printf("%s\n",p);
    p=nl+1;
  }
  return 0;
}
