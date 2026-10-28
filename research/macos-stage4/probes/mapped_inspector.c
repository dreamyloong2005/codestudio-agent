#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
 if(argc!=2)return 2;int pid=atoi(argv[1]);
 printf("sizes region=%zu vstat=%zu vnode=%zu vip=%zu result=%zu offsets vnode=%zu path=%zu\n",sizeof(struct proc_regioninfo),sizeof(struct vinfo_stat),sizeof(struct vnode_info),sizeof(struct vnode_info_path),sizeof(struct proc_regionwithpathinfo),offsetof(struct proc_regionwithpathinfo,prp_vip),offsetof(struct proc_regionwithpathinfo,prp_vip.vip_path));
 char path[4096]={0};errno=0;int n=proc_pidpath(pid,path,sizeof(path));printf("pidpath result=%d errno=%d path=%s\n",n,errno,path);
 struct stat s;if(stat(path,&s)==0)printf("pathstat dev=%u ino=%llu\n",(unsigned)s.st_dev,(unsigned long long)s.st_ino);
 uint64_t address=0;
 for(int i=0;i<4096;i++){
  struct proc_regionwithpathinfo info;memset(&info,0,sizeof(info));errno=0;n=proc_pidinfo(pid,PROC_PIDREGIONPATHINFO,address,&info,sizeof(info));
  if(n!=(int)sizeof(info)){printf("end index=%d ret=%d errno=%d\n",i,n,errno);return errno==EINVAL?0:3;}
  const struct proc_regioninfo*r=&info.prp_prinfo;const struct vinfo_stat*v=&info.prp_vip.vip_vi.vi_stat;
  if(v->vst_ino)printf("region index=%d addr=%llu size=%llu prot=%u maxprot=%u offset=%llu dev=%u ino=%llu path=%s\n",i,(unsigned long long)r->pri_address,(unsigned long long)r->pri_size,r->pri_protection,r->pri_max_protection,(unsigned long long)r->pri_offset,v->vst_dev,(unsigned long long)v->vst_ino,info.prp_vip.vip_path);
  uint64_t next=r->pri_address+r->pri_size;if(next<=address)return 4;address=next;
 }
 return 5;
}
