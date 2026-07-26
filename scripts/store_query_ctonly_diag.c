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
static int count_ct(void){int c=0;const char*p=g,*k="\"contentTopic\":\"";while((p=strstr(p,k))){c++;p+=strlen(k);}return c;}
static int status(void){const char*p=strstr(g,"\"statusCode\":");return p?atoi(p+13):-999;}
int main(int c,char**v){
 setvbuf(stdout,NULL,_IOLBF,0);
 void*h=dlopen("./liblogosdelivery.so",RTLD_NOW|RTLD_GLOBAL);
 if(!h){printf("dlopen fail %s\n",dlerror());return 1;}
 create_fn cr=(create_fn)dlsym(h,"logosdelivery_create_node");
 op_fn st=(op_fn)dlsym(h,"logosdelivery_start_node");
 sq_fn sq=(sq_fn)dlsym(h,"waku_store_query");
 if(!sq){printf("no waku_store_query\n");return 1;}
 const char*peer="/dns4/msg.logos.live/tcp/30304/p2p/16Uiu2HAmNdX1s7wRhygyWKmYiUst84329TSz3byLEP6FjcoxDbH4";
 char cfg[700];
 snprintf(cfg,sizeof(cfg),"{\"logLevel\":\"ERROR\",\"mode\":\"Edge\",\"clusterId\":2,\"numShardsInNetwork\":8,\"discv5Discovery\":false,\"staticnodes\":[\"%s\"],\"filternode\":\"%s\",\"lightpushnode\":\"%s\",\"tcpPort\":60241,\"discv5UdpPort\":60241}",peer,peer,peer);
 gd=0;void*ctx=cr(cfg,cb,NULL);if(!ctx){printf("create fail %s\n",g);return 2;}
 gd=0;st(ctx,cb,NULL);w(); printf("started, connecting...\n"); sleep(20);

 // Step 1: broad per-shard scan to find a content topic that HAS data.
 const char*shards[]={"/waku/2/rs/2/0","/waku/2/rs/2/1","/waku/2/rs/2/2","/waku/2/rs/2/3","/waku/2/rs/2/4","/waku/2/rs/2/5","/waku/2/rs/2/6","/waku/2/rs/2/7"};
 char found[256]; found[0]=0; char foundShard[64]; foundShard[0]=0;
 for(int i=0;i<8 && !found[0];i++){
   char q[400];
   snprintf(q,sizeof(q),"{\"requestId\":\"s%d\",\"includeData\":false,\"pubsubTopic\":\"%s\",\"contentTopics\":[],\"paginationLimit\":10,\"paginationForward\":false}",i,shards[i]);
   gd=0; sq(ctx,cb,NULL,q,peer,12000); w();
   printf("scan %s status=%d topics=%d\n",shards[i],status(),count_ct());
   const char*p=strstr(g,"\"contentTopic\":\"");
   if(p){p+=16;const char*e=strchr(p,'"');if(e && (e-p)<250){memcpy(found,p,e-p);found[e-p]=0;strcpy(foundShard,shards[i]);}}
 }
 if(!found[0]){printf("NO topic with data found in store\n");return 0;}
 printf("\n== picked topic: %s (on %s) ==\n",found,foundShard);

 // Step 2a: query WITH pubsubTopic + contentTopics filter (proven form)
 {char q[512];snprintf(q,sizeof(q),"{\"requestId\":\"a\",\"includeData\":false,\"pubsubTopic\":\"%s\",\"contentTopics\":[\"%s\"],\"paginationLimit\":30,\"paginationForward\":false}",foundShard,found);
  gd=0;sq(ctx,cb,NULL,q,peer,12000);w();printf("[A] pubsubTopic+contentTopics: status=%d count=%d\n",status(),count_ct());}

 // Step 2b: query contentTopics ONLY (no pubsubTopic) -- the design under test
 {char q[512];snprintf(q,sizeof(q),"{\"requestId\":\"b\",\"includeData\":false,\"contentTopics\":[\"%s\"],\"paginationLimit\":30,\"paginationForward\":false}",found);
  gd=0;sq(ctx,cb,NULL,q,peer,12000);w();printf("[B] contentTopics ONLY:      status=%d count=%d\n",status(),count_ct());
  if(status()!=200)printf("    [B] raw: %.300s\n",g);}
 return 0;
}
