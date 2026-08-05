// send_diag.c — #211: does logosdelivery_send (plain publish) SUCCEED in Edge mode
// (which disables relay -> send must go via lightpush)? If Edge send errors, Edge
// mode would fix receive but break send. Run with arg Core|Edge.
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
static int wait_cb(void){for(int i=0;i<200&&!g_done;i++)usleep(50000);return g_ret;}
int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IOLBF,0);
  const char*mode=argc>1?argv[1]:"Edge";
  const char*atopic=argc>2?argv[2]:"/kym/1/d3795c1d4f3306850dd1f422a3c962d2/proto";
  const char*b64=argc>3?argv[3]:"aGVsbG8=";
  void*h=dlopen("./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){printf("dlopen: %s\n",dlerror());return 1;}
  create_fn create=(create_fn)dlsym(h,"logosdelivery_create_node");
  op_fn start=(op_fn)dlsym(h,"logosdelivery_start_node");
  s_fn sub=(s_fn)dlsym(h,"logosdelivery_subscribe");
  s_fn send=(s_fn)dlsym(h,"logosdelivery_send");
  s_fn byproto=(s_fn)dlsym(h,"waku_get_peerids_by_protocol");
  char cfg[256];
  snprintf(cfg,sizeof(cfg),"{\"logLevel\":\"ERROR\",\"mode\":\"%s\",\"preset\":\"logos.dev\",\"tcpPort\":60170,\"discv5UdpPort\":60170}",mode);
  printf("=== mode=%s ===\n",mode);
  g_done=0;void*ctx=create(cfg,cb,NULL);if(!ctx){printf("create fail %s\n",g_buf);return 2;}
  g_done=0;start(ctx,cb,NULL);printf("start ret=%d\n",wait_cb());
  const char*topic=atopic;
  if(sub){g_done=0;sub(ctx,cb,NULL,topic);printf("subscribe ret=%d\n",wait_cb());}
  sleep(60); // longer warmup so a lightpush peer is selected
  if(byproto){g_done=0;byproto(ctx,cb,NULL,"/vac/waku/lightpush/2.0.0-beta1");wait_cb();printf("LIGHTPUSH_PEERS: %.200s\n",g_buf);}
  // payload "hello" base64 = aGVsbG8=
  char msg[512];
  snprintf(msg,sizeof(msg),"{\"contentTopic\":\"%s\",\"payload\":\"%s\",\"ephemeral\":false}",topic,b64);
  for(int k=0;k<3;k++){g_done=0;send(ctx,cb,NULL,msg);wait_cb();printf("SEND#%d ret=%d -> %.80s\n",k,g_ret,g_buf);sleep(3);}
  return 0;
}
