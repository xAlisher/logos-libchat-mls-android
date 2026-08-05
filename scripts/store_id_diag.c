#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef void(*FFICallBack)(int,const char*,size_t,void*);
typedef void*(*create_fn)(const char*,FFICallBack,void*);
typedef int(*op_fn)(void*,FFICallBack,void*);
typedef int(*sq_fn)(void*,FFICallBack,void*,const char*,const char*,int); // jsonQuery, peerAddr, timeoutMs
static char g[1<<20]; static int gr; static volatile int gd;
static void cb(int r,const char*m,size_t l,void*u){(void)u;gr=r;size_t n=(m&&l)?(l<sizeof(g)-1?l:sizeof(g)-1):0;if(n)memcpy(g,m,n);g[n]=0;gd=1;}
static int w(void){for(int i=0;i<300&&!gd;i++)usleep(50000);return gr;}
int main(int c,char**v){
 setvbuf(stdout,NULL,_IOLBF,0);
 void*h=dlopen("./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL);
 create_fn cr=(create_fn)dlsym(h,"logosdelivery_create_node");
 op_fn st=(op_fn)dlsym(h,"logosdelivery_start_node");
 sq_fn sq=(sq_fn)dlsym(h,"waku_store_query");
 if(!sq){printf("no waku_store_query symbol\n");return 1;}
 const char*cfg="{\"logLevel\":\"ERROR\",\"mode\":\"Edge\",\"clusterId\":2,\"numShardsInNetwork\":8,\"discv5Discovery\":false,\"staticnodes\":[\"/dns4/msg.logos.live/tcp/30304/p2p/16Uiu2HAmNdX1s7wRhygyWKmYiUst84329TSz3byLEP6FjcoxDbH4\"],\"tcpPort\":60240,\"discv5UdpPort\":60240}";
 gd=0;void*ctx=cr(cfg,cb,NULL);if(!ctx){printf("create fail %s\n",g);return 2;}
 gd=0;st(ctx,cb,NULL);w(); sleep(20); // connect to our node
 const char*peer="/dns4/msg.logos.live/tcp/30304/p2p/16Uiu2HAmNdX1s7wRhygyWKmYiUst84329TSz3byLEP6FjcoxDbH4";
 const char*shards[]={"/waku/2/rs/2/7","/waku/2/rs/2/0","/waku/2/rs/2/2","/waku/2/rs/2/4","/waku/2/rs/2/1","/waku/2/rs/2/3","/waku/2/rs/2/5","/waku/2/rs/2/6"};
 for(int i=0;i<8;i++){
   char q[512];
   // nwaku libwaku store query (snake_case). newest-first, data included.
   snprintf(q,sizeof(q),"{\"requestId\":\"q%d\",\"includeData\":true,\"pubsubTopic\":\"%s\",\"contentTopics\":[],\"paginationLimit\":30,\"paginationForward\":false}",i,shards[i]);
   gd=0; int r=sq(ctx,cb,NULL,q,peer,15000); (void)r; w();
   printf("=== %s ret=%d ===\n",shards[i],gr);
   {const char*p=g; const char*k="\"contentTopic\":\""; while((p=strstr(p,k))){p+=strlen(k); const char*e=strchr(p,'"'); if(!e)break; printf("  ct: %.*s\n",(int)(e-p),p); p=e;}}
 }
 return 0;
}
