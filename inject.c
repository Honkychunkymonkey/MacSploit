#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/error.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <libproc.h>
#include <sys/mman.h>

#include <sys/stat.h>
#include <pthread.h>
#include <mach/mach_vm.h>


#define STACK_SIZE 65536
#define CODE_SIZE 128

// Due to popular request:
//
// Simple injector example (and basis of coreruption tool).
//
// If you've looked into research on injection techniques in OS X, you
// probably know about mach_inject. This tool, part of Dino Dai Zovi's
// excellent "Mac Hacker's Handbook" (a must read - kudos, DDZ) was
// created to inject code in PPC and i386. Since I couldn't find anything
// for x86_64 or ARM, I ended up writing my own tool.

// Since, this tool has exploded in functionality - with many other features,
// including scriptable debugging, fault injection, function hooking, code 
// decryption,  and what not - which comes in *really* handy on iOS.
//
// coreruption is still closed source, due its highly.. uhm.. useful
// nature. But I'm making this sample free, and I have fully annotated this.
// The rest of the stuff you need is in Chapters 11 and 12 MOXiI 1, with more
// to come in the 2nd Ed (..in time for iOS 9 :-)
//
// Go forth and spread your code :-)
//
// J (info@newosxbook.com) 02/05/2014
//
// v2: With ARM64 -  06/02/2015 NOTE - ONLY FOR **ARM64**, NOT ARM32!
// Get the full bundle at - http://NewOSXBook.com/files/injarm64.tar
// with sample dylib and with script to compile this neatly.
//
//**********************************************************************
// Note ARM code IS messy, and I left the addresses wide apart. That's 
// intentional. Basic ARM64 assembly will enable you to tidy this up and
// make the code more compact. 
//
// This is *not* meant to be neat - I'm just preparing this for TG's
// upcoming OS X/iOS RE course (http://technologeeks.com/OSXRE) and thought
// this would be interesting to share. See you all in MOXiI 2nd Ed!
//**********************************************************************

// Update (7/16/2019): 
// You'll need to change pthread_set_self to from ..from_mach_thread, 
// which is required as a workaround for behavior change in Mojave (10.14) and later iOS 12
// q.v. https://knight.sc/malware/2019/03/15/code-injection-on-macos.html

// This sample code calls pthread_set_self to promote the injected thread
// to a pthread first - otherwise dlopen and many other calls (which rely
// on pthread_self()) will crash. 
// It then calls dlopen() to load the library specified - which will trigger
// the library's constructor (q.e.d as far as code injection is concerned)
// and sleep for a long time. You can of course replace the sleep with
// another function, such as pthread_exit(), etc.
//
// (For the constructor, use:
//
// static void whicheverfunc() __attribute__((constructor));
//
// in the library you inject)
//
// Note that the functions are shown here as "_PTHRDSS", "DLOPEN__" and "SLEEP___".
// Reason being, that the above are merely placeholders which will be patched with
// the runtime addresses when code is actually injected.
//
char injectedCode[] =
     //"\xcc"                           //  int3   
     "\x90"				// nop..
     "\x55"                           // pushq  %rbp
     "\x48\x89\xe5"                   // movq   %rsp, %rbp
     "\x48\x83\xec\x20"               // subq   $32, %rsp
     "\x89\x7d\xfc"                   // movl   %edi, -4(%rbp)
     "\x48\x89\x75\xf0"               // movq   %rsi, -16(%rbp)
     "\xb0\x00"                                    // movb   $0, %al
     // call pthread_set_self 
     "\x48\xbf\x00\x00\x00\x00\x00\x00\x00\x00"    // movabsq $0, %rdi
     "\x48\xb8" "_PTHRDSS"                           // movabsq $140735540045793, %rax
     "\xff\xd0"                                    //    callq  *%rax
     "\x48\xbe\x00\x00\x00\x00\x00\x00\x00\x00"    // movabsq $0, %rsi
     "\x48\x8d\x3d\x2c\x00\x00\x00"                // leaq   44(%rip), %rdi
     // DLOpen...
     "\x48\xb8" "DLOPEN__" // movabsq $140735516395848, %rax
     "\x48\xbe\x00\x00\x00\x00\x00\x00\x00\x00" //  movabsq $0, %rsi
     "\xff\xd0"                       //   callq  *%rax
     // Sleep(1000000)...
     "\x48\xbf\x00\xe4\x0b\x54\x02\x00\x00\x00" //  movabsq $10000000000, %rdi
     "\x48\xb8" "SLEEP___" // movabsq $140735516630165, %rax
     "\xff\xd0"            //              callq  *%rax

     // plenty of space for a full path name here
     "LIBLIBLIBLIB" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
;


int inject(pid_t pid, const char *lib) {

task_t remoteTask;

struct stat buf;

/**
  * First, check we have the library. Otherwise, we won't be able to inject..
  */

  int rc = stat (lib, &buf);

  if (rc != 0)
  {
   fprintf (stderr, "Unable to open library file %s (%s) - Cannot inject\n", lib,strerror (errno));
   //return (-9);
   }

mach_error_t kr = 0;

/**
  * Second - the critical part - we need task_for_pid in order to get the task port of the target
  * pid. This is our do-or-die: If we get the port, we can do *ANYTHING* we want. If we don't, we're
  * #$%#$%. 
  *
  * In iOS, this will require the task_for_pid-allow entitlement. In OS X, this will require getting past
  * taskgated, but root access suffices for that.
  *
  */
kr = task_for_pid(mach_task_self(), pid, &remoteTask);
if (kr != KERN_SUCCESS) {

	fprintf (stderr, "Unable to call task_for_pid on pid %d: %s. Cannot continue!\n",pid, mach_error_string(kr));
	return (-1);
}



 


/**
 * From here on, it's pretty much straightforward -
 * Allocate stack and code. We don't really care *where* they get allocated. Just that they get allocated.
 * So, first, stack:
 */
mach_vm_address_t remoteStack64 = (vm_address_t) NULL;
mach_vm_address_t remoteCode64 = (vm_address_t) NULL;
kr = mach_vm_allocate( remoteTask, &remoteStack64, STACK_SIZE, VM_FLAGS_ANYWHERE);
   
if (kr != KERN_SUCCESS)
	{
		fprintf(stderr,"Unable to allocate memory for remote stack in thread: Error %s\n", mach_error_string(kr));
		return (-2);
	}
else
{

	fprintf (stderr, "Allocated remote stack @0x%llx\n", remoteStack64);

}
/**
 * Then we allocate the memory for the thread
 */
remoteCode64 = (vm_address_t) NULL;
kr = mach_vm_allocate( remoteTask, &remoteCode64, CODE_SIZE, VM_FLAGS_ANYWHERE );

if (kr != KERN_SUCCESS)
	{
		fprintf(stderr,"Unable to allocate memory for remote code in thread: Error %s\n", mach_error_string(kr));
		return (-2);
	}


 
 /**
   * Patch code before injecting: That is, insert correct function addresses (and lib name) into placeholders
   *
   * Since we use the same shared library cache as our victim, meaning we can use memory addresses from
   * OUR address space when we inject..
   */

 int i = 0;
 char *possiblePatchLocation = (injectedCode );
 for (i = 0 ; i < 0x100; i++)
  {

	// Patching is crude, but works.
  	//
	extern void *_pthread_set_self;
	possiblePatchLocation++;

	
	uint64_t addrOfPthreadSetSelf = dlsym ( RTLD_DEFAULT, "_pthread_set_self"); //(uint64_t) _pthread_set_self;
	uint64_t addrOfPthreadExit = dlsym (RTLD_DEFAULT, "pthread_exit"); //(uint64_t) _pthread_set_self;
        uint64_t addrOfDlopen = (uint64_t) dlopen;
        uint64_t addrOfSleep = (uint64_t) sleep; // pthread_exit;

	if (memcmp (possiblePatchLocation, "PTHRDEXT", 8) == 0)
	{
	   memcpy(possiblePatchLocation, &addrOfPthreadExit,8);

	   printf ("Pthread exit  @%llx, %llx\n", addrOfPthreadExit, pthread_exit);
	}

	if (memcmp (possiblePatchLocation, "_PTHRDSS", 8) == 0)
	{
	   memcpy(possiblePatchLocation, &addrOfPthreadSetSelf,8);

	   printf ("Pthread set self @%llx\n", addrOfPthreadSetSelf);
	}

	if (memcmp(possiblePatchLocation, "DLOPEN__", 6) == 0)
	{
	   printf ("DLOpen @%llx\n", addrOfDlopen);
	   memcpy(possiblePatchLocation, &addrOfDlopen, sizeof(uint64_t));

	}

	if (memcmp(possiblePatchLocation, "SLEEP___", 6) == 0)
	{
	   printf ("Sleep @%llx\n", addrOfSleep);
	   memcpy(possiblePatchLocation, &addrOfSleep, sizeof(uint64_t));

	}

	if (memcmp(possiblePatchLocation, "LIBLIBLIB", 9) == 0)
	{

	   strcpy(possiblePatchLocation, lib );

	}
	




  }

	/**
  	  * Write the (now patched) code
	  */
	kr = mach_vm_write(remoteTask,                   // Task port
	                   remoteCode64,                 // Virtual Address (Destination)
	                   (vm_address_t) injectedCode,  // Source
	                    0xa9);                       // Length of the source



       if (kr != KERN_SUCCESS)
	{
		fprintf(stderr,"Unable to write remote thread memory: Error %s\n", mach_error_string(kr));
		return (-3);
	}


        /*
	 * Mark code as executable - This also requires a workaround on iOS, btw.
	 */
	
        kr  = vm_protect(remoteTask, remoteCode64, 0x70, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);

	/*
   	 * Mark stack as writable  - not really necessary 
	 */

        kr  = vm_protect(remoteTask, remoteStack64, STACK_SIZE, TRUE, VM_PROT_READ | VM_PROT_WRITE);
	

        if (kr != KERN_SUCCESS)
	{
		fprintf(stderr,"Unable to set memory permissions for remote thread: Error %s\n", mach_error_string(kr));
		return (-4);
	}


        /**
 	  *
 	  * Create thread - This is obviously hardware specific.  
	  *
	  */

x86_thread_state64_t remoteThreadState64;
        thread_act_t         remoteThread;

        memset(&remoteThreadState64, '\0', sizeof(remoteThreadState64) );

        remoteStack64 += (STACK_SIZE / 2); // this is the real stack
	//remoteStack64 -= 8;  // need alignment of 16

        const char* p = (const char*) remoteCode64;
        remoteThreadState64.__rip = (u_int64_t) (vm_address_t) remoteCode64;

        // set remote Stack Pointer
        remoteThreadState64.__rsp = (u_int64_t) remoteStack64;
        remoteThreadState64.__rbp = (u_int64_t) remoteStack64;

	printf ("Remote Stack 64  0x%llx, Remote code is %p\n", remoteStack64, p );

	/*
	 * create thread and launch it in one go
	 */
kr = thread_create_running( remoteTask, x86_THREAD_STATE64,
(thread_state_t) &remoteThreadState64, x86_THREAD_STATE64_COUNT, &remoteThread );
if (kr != KERN_SUCCESS) { fprintf(stderr,"Unable to create remote thread: error %s", mach_error_string (kr));
			  return (-3); }

return (0);

} // end injection code

#if 0


tatic void con() __attribute__((constructor));

void con() {

    printf("I'm a constructor\n");

}

#endif

int find_roblox_pid() {
    pid_t pid_list[512 * sizeof(pid_t)];
    struct proc_bsdinfo pinfo;
    int byte_count = proc_listpids(PROC_ALL_PIDS, 0, pid_list, 2048);
    int proc_count = byte_count / 4;
    for (int i = 0; i < proc_count; i++) {
        int read = proc_pidinfo(pid_list[i], PROC_PIDTBSDINFO, 0, &pinfo, PROC_PIDTBSDINFO_SIZE);
        if (read == PROC_PIDTBSDINFO_SIZE) {
            if (strcmp(pinfo.pbi_name, "RobloxPlayer") == 0) {
                return pid_list[i];
            }
        }
    }

    return 0;
}

int main() {
    char path[128];
    printf("What is the path to the dylib?\n");
    scanf("%s", path);
    int pid = find_roblox_pid();
    inject(pid, path);
    return 0;
}