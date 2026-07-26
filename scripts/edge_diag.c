// edge_diag.c — #211 FIX validation: does mode=Edge (filter client, no relay mesh)
// hold a stable filter service peer where Core/relay mode's gossipsub mesh
// collapsed to 0? If yes, Edge mode is the durable delivery fix for the mobile
// node. dlopens liblogosdelivery.so directly (no app/Rust rebuild).
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef void (*FFICallBack)(int, const char *, size_t, void *);
typedef void *(*create_fn)(const char *, FFICallBack, void *);
typedef int (*op_fn)(void *, FFICallBack, void *);
typedef int (*sub_fn)(void *, FFICallBack, void *, const char *);
typedef int (*proto_fn)(void *, FFICallBack, void *, const char *);
static char g_buf[65536]; static int g_ret; static volatile int g_done;
static void cb(int r,const char*m,size_t l,void*u){(void)u;g_ret=r;size_t n=(m&&l)?(l<sizeof(g_buf)-1?l:sizeof(g_buf)-1):0;if(n)memcpy(g_buf,m,n);g_buf[n]=0;g_done=1;}
static int wait_cb(void){for(int i=0;i<200&&!g_done;i++)usleep(50000);return g_ret;}
static void call(proto_fn f,void*ctx,const char*a,const char*label){
  if(!f){printf("  %-34s <missing>\n",label);return;}
  g_done=0;f(ctx,cb,NULL,a);wait_cb();printf("  %-34s ret=%d -> %s\n",label,g_ret,g_buf);
}
int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IOLBF,0);
  const char*mode=argc>1?argv[1]:"Edge";
  void*h=dlopen("./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){printf("dlopen: %s\n",dlerror());return 1;}
  create_fn create=(create_fn)dlsym(h,"logosdelivery_create_node");
  op_fn start=(op_fn)dlsym(h,"logosdelivery_start_node");
  sub_fn sub=(sub_fn)dlsym(h,"logosdelivery_subscribe");
  proto_fn byproto=(proto_fn)dlsym(h,"waku_get_peerids_by_protocol");
  proto_fn nconn=(proto_fn)dlsym(h,"waku_relay_get_num_connected_peers");
  proto_fn nmesh=(proto_fn)dlsym(h,"waku_relay_get_num_peers_in_mesh");
  char cfg[256];
  snprintf(cfg,sizeof(cfg),"{\"logLevel\":\"ERROR\",\"mode\":\"%s\",\"preset\":\"logos.dev\",\"tcpPort\":60160,\"discv5UdpPort\":60160}",mode);
  printf("=== mode=%s ===\n",mode);
  g_done=0;void*ctx=create(cfg,cb,NULL);
  printf("create ret=%d ctx=%p msg=%.120s\n",g_ret,ctx,g_buf);
  if(!ctx)return 2;
  g_done=0;start(ctx,cb,NULL);printf("start ret=%d %.120s\n",wait_cb(),g_buf);
  if(sub){g_done=0;sub(ctx,cb,NULL,"/kym/1/d3795c1d4f3306850dd1f422a3c962d2/proto");printf("subscribe ret=%d %.80s\n",wait_cb(),g_buf);}
  for(int t=0;t<6;t++){
    printf("--- t=%ds ---\n",t*15);
    call(nconn,ctx,"","num_connected_peers(all)");
    call(byproto,ctx,"/vac/waku/filter-subscribe/2.0.0-beta1","filter_service_peers");
    call(nmesh,ctx,"/waku/2/rs/2/7","mesh_peers(rs/2/7)");
    sleep(15);
  }
  return 0;
}
