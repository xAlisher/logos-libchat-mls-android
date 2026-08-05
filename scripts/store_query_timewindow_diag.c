#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef void(*FFICallBack)(int,const char*,size_t,void*);
typedef void*(*create_fn)(const char*,FFICallBack,void*);
typedef int(*op_fn)(void*,FFICallBack,void*);
typedef int(*sq_fn)(void*,FFICallBack,void*,const char*,const char*,int);
static char g[1<<20]; static int gr; static volatile int gd;
static void cb(int r,const char*m,size_t l,void*u){(void)u;gr=r;size_t n=(m&&l)?(l<sizeof(g)-1?l:sizeof(g)-1):0;if(n)memcpy(g,m,n);g[n]=0;gd=1;}
static int w(void){for(int i=0;i<400&&!gd;i++)usleep(50000);return gr;}
static int cnt(void){int c=0;const char*p=g,*k="\"messageHash\":\"";while((p=strstr(p,k))){c++;p+=strlen(k);}return c;}
static int status(void){const char*p=strstr(g,"\"statusCode\":");return p?atoi(p+13):-999;}
int main(){
 setvbuf(stdout,NULL,_IOLBF,0);
 void*h=dlopen("./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL); if(!h){printf("dlopen %s\n",dlerror());return 1;}
 create_fn cr=(create_fn)dlsym(h,"logosdelivery_create_node");
 op_fn st=(op_fn)dlsym(h,"logosdelivery_start_node");
 sq_fn sq=(sq_fn)dlsym(h,"waku_store_query");
 const char*peer="/dns4/msg.logos.live/tcp/30304/p2p/16Uiu2HAmNdX1s7wRhygyWKmYiUst84329TSz3byLEP6FjcoxDbH4";
 char cfg[700]; snprintf(cfg,sizeof(cfg),"{\"logLevel\":\"ERROR\",\"mode\":\"Edge\",\"clusterId\":2,\"numShardsInNetwork\":8,\"discv5Discovery\":false,\"staticnodes\":[\"%s\"],\"tcpPort\":60242,\"discv5UdpPort\":60242}",peer);
 gd=0;void*ctx=cr(cfg,cb,NULL);if(!ctx){printf("create %s\n",g);return 2;}
 gd=0;st(ctx,cb,NULL);w();printf("connecting 25s...\n");sleep(25);
 const char*T="/kym/1/d3795c1d4f3306850dd1f422a3c962d2/proto"; // 28k msgs, shard 2
 // A: no time bounds, with pubsubTopic
 {char q[600];snprintf(q,sizeof(q),"{\"requestId\":\"a\",\"includeData\":false,\"pubsubTopic\":\"/waku/2/rs/2/2\",\"contentTopics\":[\"%s\"],\"paginationLimit\":20,\"paginationForward\":false}",T);
  gd=0;sq(ctx,cb,NULL,q,peer,15000);w();printf("[A] no-time pubsub+ct: status=%d count=%d\n",status(),cnt());if(status()!=200)printf("  raw:%.200s\n",g);}
 // B: 24h time window, with pubsubTopic
 {char q[700];snprintf(q,sizeof(q),"{\"requestId\":\"b\",\"includeData\":false,\"pubsubTopic\":\"/waku/2/rs/2/2\",\"contentTopics\":[\"%s\"],\"timeStart\":1785002252000000000,\"timeEnd\":1785092252000000000,\"paginationLimit\":20,\"paginationForward\":false}",T);
  gd=0;sq(ctx,cb,NULL,q,peer,15000);w();printf("[B] 24h pubsub+ct:    status=%d count=%d\n",status(),cnt());if(status()!=200)printf("  raw:%.200s\n",g);}
 // C: contentTopics only + 24h window
 {char q[600];snprintf(q,sizeof(q),"{\"requestId\":\"c\",\"includeData\":false,\"contentTopics\":[\"%s\"],\"timeStart\":1785002252000000000,\"timeEnd\":1785092252000000000,\"paginationLimit\":20,\"paginationForward\":false}",T);
  gd=0;sq(ctx,cb,NULL,q,peer,15000);w();printf("[C] 24h ct-only:      status=%d count=%d\n",status(),cnt());if(status()!=200)printf("  raw:%.200s\n",g);}
 return 0;
}
