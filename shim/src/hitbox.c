// hitbox.c — hitbox / bounding-volume overlay for Halo 3 objects. Included by peload.c.
// The retail halo3.dll compiled out its own debug_objects_* overlays (see shim/docs/halo3-engine-globals.md),
// so we find the engine's object table in memory and draw the volumes ourselves.
//
// Step 1 (PELOAD_OBJSCAN=1): halo3 keeps entities in "data arrays" whose header carries a name and the
// signature 'd@t@'. Scan readable memory a few seconds after gameplay starts and print every data array
// found (name, capacity, element size), to locate the object table.
#include <mach/mach.h>

static void objscan_dump(void){
    mach_vm_address_t a=0x10000; char m[400]; int k, found=0;
    for(;;){ mach_vm_size_t sz=0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt=VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if(mach_vm_region(mach_task_self(),&a,&sz,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&cnt,&obj)!=KERN_SUCCESS) break;
        if((info.protection&VM_PROT_READ) && sz<(1ull<<34) && a<0x800000000000ULL){
            const uint8_t*p=(const uint8_t*)(uintptr_t)a, *end=p+sz;
            while(p<end){ const uint8_t*h=memmem(p,(size_t)(end-p),"d@t@",4); if(!h) break;
                // the name is a NUL-terminated string at the start of the header (look back up to 0x40 bytes)
                const uint8_t*hb=h; int best=-1;
                for(int back=0x20; back<=0x40; back+=4){ if(h-back<(const uint8_t*)(uintptr_t)a) break; const uint8_t*n=h-back;
                    int ok=n[0]>='a'&&n[0]<='z'; for(int i=0;i<32&&n[i]&&ok;i++) if(n[i]<32||n[i]>126) ok=0; if(ok){ best=back; hb=n; break; } }
                if(best>0 && found<200){ char nm[40]; snprintf(nm,sizeof nm,"%.32s",(const char*)hb);
                    const int32_t*q=(const int32_t*)(hb+32);
                    k=snprintf(m,sizeof m,"  [objscan] %p name='%s' sig@+0x%x ints: %d %d %d %d %d %d %d %d\n",(void*)hb,nm,best,q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7]); (void)!write(2,m,k); found++; }
                p=h+4; } }
        a+=sz; }
    k=snprintf(m,sizeof m,"  [objscan] %d data arrays\n",found); (void)!write(2,m,k); }
static void* objscan_thread(void*arg){ (void)arg; extern double g_gameplay_t0; while(!g_gameplay_t0) usleep(200000); sleep(5); objscan_dump(); return 0; }
static void hitbox_start(void){
    if(getenv("PELOAD_OBJSCAN")){ pthread_t t; pthread_create(&t,0,objscan_thread,0); } }
