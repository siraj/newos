/*
** Copyright 2001-2004, Mark-Jan Bastian. All rights reserved.
** Distributed under the terms of the NewOS License.
*/

#include <kernel/kernel.h>
#include <kernel/port.h>
#include <kernel/sem.h>
#include <kernel/int.h>
#include <kernel/debug.h>
#include <kernel/heap.h>
#include <kernel/vm.h>
#include <kernel/cbuf.h>
#include <newos/errors.h>

#include <string.h>
#include <stdlib.h>

struct port_msg {
	int		msg_code;
	cbuf*	data_cbuf;
	size_t	data_len;
};

struct port_entry {
	port_id 			id;
	proc_id 			owner;
	int32 				capacity;
	int     			lock;
	char				*name;
	sem_id				read_sem;
	sem_id				write_sem;
	int					head;
	int					tail;
	int					total_count;
	bool				closed;
	struct port_msg*	msg_queue;
};

// internal API
void dump_port_list(int argc, char **argv);
static void _dump_port_info(struct port_entry *port);
static void dump_port_info(int argc, char **argv);


// MAX_PORTS must be power of 2
#define MAX_PORTS 4096
#define MAX_QUEUE_LENGTH 4096
#define PORT_MAX_MESSAGE_SIZE 65536

static struct port_entry *ports = NULL;
static region_id port_region = 0;
static bool ports_active = false;

static port_id next_port = 0;

static int port_spinlock = 0;
#define GRAB_PORT_LIST_LOCK() acquire_spinlock(&port_spinlock)
#define RELEASE_PORT_LIST_LOCK() release_spinlock(&port_spinlock)
#define GRAB_PORT_LOCK(s) acquire_spinlock(&(s).lock)
#define RELEASE_PORT_LOCK(s) release_spinlock(&(s).lock)

int port_init(kernel_args *ka)
{
	int i;
	int sz;

	sz = sizeof(struct port_entry) * MAX_PORTS;

	// create and initialize semaphore table
	port_region = vm_create_anonymous_region(vm_get_kernel_aspace_id(), "port_table", (void **)&ports,
		REGION_ADDR_ANY_ADDRESS, sz, REGION_WIRING_WIRED, LOCK_RW|LOCK_KERNEL);
	if(port_region < 0) {
		panic("unable to allocate kernel port table!\n");
	}

	memset(ports, 0, sz);
	for(i=0; i<MAX_PORTS; i++)
		ports[i].id = -1;

	// add debugger commands
	dbg_add_command(&dump_port_list, "ports", "Dump a list of all active ports");
	dbg_add_command(&dump_port_info, "port", "Dump info about a particular port");

	ports_active = true;

	return 0;
}

void dump_port_list(int argc, char **argv)
{
	int i;

	for(i=0; i<MAX_PORTS; i++) {
		if(ports[i].id >= 0) {
			dprintf("%p\tid: 0x%x\t\tname: '%s'\n", &ports[i], ports[i].id, ports[i].name);
		}
	}
}

static void _dump_port_info(struct port_entry *port)
{
	int cnt;
	dprintf("PORT:   %p\n", port);
	dprintf("name:  '%s'\n", port->name);
	dprintf("owner: 0x%x\n", port->owner);
	dprintf("cap:  %d\n", port->capacity);
	dprintf("head: %d\n", port->head);
	dprintf("tail: %d\n", port->tail);
 	sem_get_count(port->read_sem, &cnt);
 	dprintf("read_sem:  %d\n", cnt);
 	sem_get_count(port->write_sem, &cnt);
	dprintf("write_sem: %d\n", cnt);
}

static void dump_port_info(int argc, char **argv)
{
	int i;

	if(argc < 2) {
		dprintf("port: not enough arguments\n");
		return;
	}

	// if the argument looks like a hex number, treat it as such
	if(strlen(argv[1]) > 2 && argv[1][0] == '0' && argv[1][1] == 'x') {
		unsigned long num = atoul(argv[1]);

		if(is_kernel_address(num)) {
			// XXX semi-hack
			// one can use either address or a port_id, since KERNEL_BASE > MAX_PORTS assumed
			_dump_port_info((struct port_entry *)num);
			return;
		} else {
			unsigned slot = num % MAX_PORTS;
			if(ports[slot].id != (int)num) {
				dprintf("port 0x%lx doesn't exist!\n", num);
				return;
			}
			_dump_port_info(&ports[slot]);
			return;
		}
	}

	// walk through the ports list, trying to match name
	for(i=0; i<MAX_PORTS; i++) {
		if (ports[i].name != NULL)
			if(strcmp(argv[1], ports[i].name) == 0) {
				_dump_port_info(&ports[i]);
				return;
			}
	}
}

port_id
port_create(int32 queue_length, const char *name)
{
	int 	i;
	sem_id 	sem_r, sem_w;
	port_id retval;
	char 	*temp_name;
	int 	name_len;
	void 	*q;
	proc_id	owner;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;

	if(name == NULL)
		name = "unnamed port";

	name_len = strlen(name) + 1;
	name_len = min(name_len, SYS_MAX_OS_NAME_LEN);

	temp_name = (char *)kmalloc(name_len);
	if(temp_name == NULL)
		return ERR_NO_MEMORY;

	strlcpy(temp_name, name, name_len);

	// check queue length
	if (queue_length < 1 || queue_length > MAX_QUEUE_LENGTH) {
		kfree(temp_name);
		return ERR_INVALID_ARGS;
	}

	// alloc a queue
	q = kmalloc( queue_length * sizeof(struct port_msg) );
	if (q == NULL) {
		kfree(temp_name); // dealloc name, too
		return ERR_NO_MEMORY;
	}

	// create sem_r with owner set to -1
	sem_r = sem_create_etc(0, temp_name, -1);
	if (sem_r < 0) {
		// cleanup
		kfree(temp_name);
		kfree(q);
		return sem_r;
	}

	// create sem_w
	sem_w = sem_create_etc(queue_length, temp_name, -1);
	if (sem_w < 0) {
		// cleanup
		sem_delete(sem_r);
		kfree(temp_name);
		kfree(q);
		return sem_w;
	}
	owner = proc_get_current_proc_id();

	int_disable_interrupts();
	GRAB_PORT_LIST_LOCK();

	// find the first empty spot
	for(i=0; i<MAX_PORTS; i++) {
		if(ports[i].id == -1) {
			// make the port_id be a multiple of the slot it's in
			if(i >= next_port % MAX_PORTS) {
				next_port += i - next_port % MAX_PORTS;
			} else {
				next_port += MAX_PORTS - (next_port % MAX_PORTS - i);
			}
			ports[i].id		= next_port++;
			ports[i].lock	= 0;
			GRAB_PORT_LOCK(ports[i]);
			RELEASE_PORT_LIST_LOCK();

			ports[i].capacity	= queue_length;
			ports[i].name 		= temp_name;

			// assign sem
			ports[i].read_sem	= sem_r;
			ports[i].write_sem	= sem_w;
			ports[i].msg_queue	= q;
			ports[i].head 		= 0;
			ports[i].tail 		= 0;
			ports[i].total_count= 0;
			ports[i].owner 		= owner;
			retval = ports[i].id;
			RELEASE_PORT_LOCK(ports[i]);
			goto out;
		}
	}
	// not enough ports...
	RELEASE_PORT_LIST_LOCK();
	kfree(q);
	kfree(temp_name);
	retval = ERR_PORT_OUT_OF_SLOTS;
	dprintf("port_create(): ERR_PORT_OUT_OF_SLOTS\n");

	// cleanup
	sem_delete(sem_w);
	sem_delete(sem_r);
	kfree(temp_name);
	kfree(q);

out:
	int_restore_interrupts();

	return retval;
}

int
port_close(port_id id)
{
	int		slot;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;
	slot = id % MAX_PORTS;

	// walk through the sem list, trying to match name
	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if (ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		return ERR_INVALID_HANDLE;
	}

	// mark port to disable writing
	ports[slot].closed = true;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	return NO_ERROR;
}

int
port_delete(port_id id)
{
	int slot;
	sem_id	r_sem, w_sem;
	int capacity;
	int i;

	char *old_name;
	struct port_msg *q;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;

	slot = id % MAX_PORTS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("port_delete: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}

	/* mark port as invalid */
	ports[slot].id	 = -1;
	old_name 		 = ports[slot].name;
	q				 = ports[slot].msg_queue;
	r_sem			 = ports[slot].read_sem;
	w_sem			 = ports[slot].write_sem;
	capacity		 = ports[slot].capacity;
	ports[slot].name = NULL;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// delete the cbuf's that are left in the queue (if any)
	for (i=0; i<capacity; i++) {
		if (q[i].data_cbuf != NULL)
	 		cbuf_free_chain(q[i].data_cbuf);
	}

	kfree(q);
	kfree(old_name);

	// release the threads that were blocking on this port by deleting the sem
	// read_port() will see the ERR_SEM_DELETED acq_sem() return value, and act accordingly
	sem_delete(r_sem);
	sem_delete(w_sem);

	return NO_ERROR;
}

port_id
port_find(const char *port_name)
{
	int i;
	int ret_val = ERR_INVALID_HANDLE;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(port_name == NULL)
		return ERR_INVALID_HANDLE;

	// lock list of ports
	int_disable_interrupts();
	GRAB_PORT_LIST_LOCK();

	// loop over list
	for(i=0; i<MAX_PORTS; i++) {
		// lock every individual port before comparing
		GRAB_PORT_LOCK(ports[i]);
		if(ports[i].id >= 0 && strcmp(port_name, ports[i].name) == 0) {
			ret_val = ports[i].id;
			RELEASE_PORT_LOCK(ports[i]);
			break;
		}
		RELEASE_PORT_LOCK(ports[i]);
	}

	RELEASE_PORT_LIST_LOCK();
	int_restore_interrupts();

	return ret_val;
}

int
port_get_info(port_id id, struct port_info *info)
{
	int slot;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if (info == NULL)
		return ERR_INVALID_ARGS;
	if(id < 0)
		return ERR_INVALID_HANDLE;

	slot = id % MAX_PORTS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("port_get_info: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}

	// fill a port_info struct with info
	info->id			= ports[slot].id;
	info->owner 		= ports[slot].owner;
	strncpy(info->name, ports[slot].name, min(strlen(ports[slot].name),SYS_MAX_OS_NAME_LEN-1));
	info->capacity		= ports[slot].capacity;
	sem_get_count(ports[slot].read_sem, &info->queue_count);
	info->total_count	= ports[slot].total_count;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// from our port_entry
	return NO_ERROR;
}

int
port_get_next_port_info(proc_id proc,
						uint32 *cookie,
						struct port_info *info)
{
	int slot;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if (cookie == NULL)
		return ERR_INVALID_ARGS;

	if (*cookie == NULL) {
		// return first found
		slot = 0;
	} else {
		// start at index cookie, but check cookie against MAX_PORTS
		slot = *cookie;
		if (slot >= MAX_PORTS)
			return ERR_INVALID_HANDLE;
	}

	// spinlock
	int_disable_interrupts();
	GRAB_PORT_LIST_LOCK();

	info->id = -1; // used as found flag
	while (slot < MAX_PORTS) {
		GRAB_PORT_LOCK(ports[slot]);
		if (ports[slot].id != -1)
			if (ports[slot].owner == proc) {
				// found one!
				// copy the info
				info->id			= ports[slot].id;
				info->owner 		= ports[slot].owner;
				strncpy(info->name, ports[slot].name, min(strlen(ports[slot].name),SYS_MAX_OS_NAME_LEN-1));
				info->capacity		= ports[slot].capacity;
				sem_get_count(ports[slot].read_sem, &info->queue_count);
				info->total_count	= ports[slot].total_count;
				RELEASE_PORT_LOCK(ports[slot]);
				slot++;
				break;
			}
		RELEASE_PORT_LOCK(ports[slot]);
		slot++;
	}
	RELEASE_PORT_LIST_LOCK();
	int_restore_interrupts();

	if (info->id == -1)
		return ERR_PORT_NOT_FOUND;
	*cookie = slot;
	return NO_ERROR;
}

ssize_t
port_buffer_size(port_id id)
{
	return port_buffer_size_etc(id, 0, 0);
}

ssize_t
port_buffer_size_etc(port_id id,
					uint32 flags,
					bigtime_t timeout)
{
	int slot;
	int res;
	int t;
	int len;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;

	slot = id % MAX_PORTS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("port_get_info: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}
	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// block if no message,
	// if TIMEOUT flag set, block with timeout

	// XXX - is it a race condition to acquire a sem just after we
	// unlocked the port ?
	// XXX: call an acquire_sem which does the release lock, restore int & block the right way
	res = sem_acquire_etc(ports[slot].read_sem, 1, flags & (SEM_FLAG_TIMEOUT | SEM_FLAG_INTERRUPTABLE), timeout, NULL);

	GRAB_PORT_LOCK(ports[slot]);
	if (res == ERR_SEM_DELETED) {
		// somebody deleted the port
		RELEASE_PORT_LOCK(ports[slot]);
		return ERR_PORT_DELETED;
	}
	if (res == ERR_SEM_TIMED_OUT) {
		RELEASE_PORT_LOCK(ports[slot]);
		return ERR_PORT_TIMED_OUT;
	}

	// once message arrived, read data's length

	// determine tail
	// read data's head length
	t = ports[slot].head;
	if (t < 0)
		panic("port %id: tail < 0", ports[slot].id);
	if (t > ports[slot].capacity)
		panic("port %id: tail > cap %d", ports[slot].id, ports[slot].capacity);
	len = ports[slot].msg_queue[t].data_len;

	// restore readsem
	sem_release(ports[slot].read_sem, 1);

	RELEASE_PORT_LOCK(ports[slot]);

	// return length of item at end of queue
	return len;
}

int32
port_count(port_id id)
{
	int slot;
	int count;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;

	slot = id % MAX_PORTS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("port_count: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}

	sem_get_count(ports[slot].read_sem, &count);
	// do not return negative numbers
	if (count < 0)
		count = 0;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// return count of messages (sem_count)
	return count;
}

ssize_t
port_read(port_id port,
			int32 *msg_code,
			void *msg_buffer,
			size_t buffer_size)
{
	return port_read_etc(port, msg_code, msg_buffer, buffer_size, 0, 0);
}

ssize_t
port_read_etc(port_id id,
				int32	*msg_code,
				void	*msg_buffer,
				size_t	buffer_size,
				uint32	flags,
				bigtime_t	timeout)
{
	int		slot;
	sem_id	cached_semid;
	size_t 	siz;
	int		res;
	int		t;
	cbuf*	msg_store;
	int32	code;
	int		err;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;
	if(msg_code == NULL)
		return ERR_INVALID_ARGS;
	if((msg_buffer == NULL) && (buffer_size > 0))
		return ERR_INVALID_ARGS;
	if (timeout < 0)
		return ERR_INVALID_ARGS;

	flags = flags & (PORT_FLAG_USE_USER_MEMCPY | PORT_FLAG_INTERRUPTABLE | PORT_FLAG_TIMEOUT);

	slot = id % MAX_PORTS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("read_port_etc: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}
	// store sem_id in local variable
	cached_semid = ports[slot].read_sem;

	// unlock port && enable ints/
	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// XXX -> possible race condition if port gets deleted (->sem deleted too), therefore
	// sem_id is cached in local variable up here

	// get 1 entry from the queue, block if needed
	res = sem_acquire_etc(cached_semid, 1,
						flags, timeout, NULL);

	// XXX: possible race condition if port read by two threads...
	//      both threads will read in 2 different slots allocated above, simultaneously
	// 		slot is a thread-local variable

	if (res == ERR_SEM_DELETED) {
		// somebody deleted the port
		return ERR_PORT_DELETED;
	}

	if (res == ERR_INTERRUPTED) {
		// XXX: somebody signaled the process the port belonged to, deleting the sem ?
		return ERR_INTERRUPTED;
	}

	if (res == ERR_SEM_TIMED_OUT) {
		// timed out, or, if timeout=0, 'would block'
		return ERR_PORT_TIMED_OUT;
	}

	if (res != NO_ERROR) {
		dprintf("write_port_etc: res unknown error %d\n", res);
		return res;
	}

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	t = ports[slot].tail;
	if (t < 0)
		panic("port %id: tail < 0", ports[slot].id);
	if (t > ports[slot].capacity)
		panic("port %id: tail > cap %d", ports[slot].id, ports[slot].capacity);

	ports[slot].tail = (ports[slot].tail + 1) % ports[slot].capacity;

	msg_store	= ports[slot].msg_queue[t].data_cbuf;
	code 		= ports[slot].msg_queue[t].msg_code;

	// mark queue entry unused
	ports[slot].msg_queue[t].data_cbuf	= NULL;

	// check output buffer size
	siz	= min(buffer_size, ports[slot].msg_queue[t].data_len);

	cached_semid = ports[slot].write_sem;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// copy message
	*msg_code = code;
	if (siz > 0) {
		if (flags & PORT_FLAG_USE_USER_MEMCPY) {
			if ((err = cbuf_user_memcpy_from_chain(msg_buffer, msg_store, 0, siz) < 0))	{
				// leave the port intact, for other threads that might not crash
				cbuf_free_chain(msg_store);
				sem_release(cached_semid, 1);
				return err;
			}
		} else
			cbuf_memcpy_from_chain(msg_buffer, msg_store, 0, siz);
	}
	// free the cbuf
	cbuf_free_chain(msg_store);

	// make one spot in queue available again for write
	sem_release(cached_semid, 1);

	return siz;
}

int
port_set_owner(port_id id, proc_id proc)
{
	int slot;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;

	slot = id % MAX_PORTS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("port_set_owner: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}

	// transfer ownership to other process
	ports[slot].owner = proc;

	// unlock port
	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	return NO_ERROR;
}

int
port_write(port_id id,
	int32 msg_code,
	void *msg_buffer,
	size_t buffer_size)
{
	return port_write_etc(id, msg_code, msg_buffer, buffer_size, 0, 0);
}

int
port_write_etc(port_id id,
	int32 msg_code,
	void *msg_buffer,
	size_t buffer_size,
	uint32 flags,
	bigtime_t timeout)
{
	int slot;
	int res;
	sem_id cached_semid;
	int h;
	cbuf* msg_store;
	int c1, c2;
	int err;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;
	if(id < 0)
		return ERR_INVALID_HANDLE;

	// mask irrelevant flags
	flags = flags & (PORT_FLAG_USE_USER_MEMCPY | PORT_FLAG_INTERRUPTABLE | PORT_FLAG_TIMEOUT);

	slot = id % MAX_PORTS;

	// check buffer_size
	if (buffer_size > PORT_MAX_MESSAGE_SIZE)
		return ERR_INVALID_ARGS;

	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	if(ports[slot].id != id) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("write_port_etc: invalid port_id %d\n", id);
		return ERR_INVALID_HANDLE;
	}

	if (ports[slot].closed) {
		RELEASE_PORT_LOCK(ports[slot]);
		int_restore_interrupts();
		dprintf("write_port_etc: port %d closed\n", id);
		return ERR_PORT_CLOSED;
	}

	// store sem_id in local variable
	cached_semid = ports[slot].write_sem;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	// XXX -> possible race condition if port gets deleted (->sem deleted too),
	// and queue is full therefore sem_id is cached in local variable up here

	// get 1 entry from the queue, block if needed
	// assumes flags
	res = sem_acquire_etc(cached_semid, 1,
						flags & (SEM_FLAG_TIMEOUT | SEM_FLAG_INTERRUPTABLE), timeout, NULL);

	// XXX: possible race condition if port written by two threads...
	//      both threads will write in 2 different slots allocated above, simultaneously
	// 		slot is a thread-local variable

	if (res == ERR_SEM_DELETED) {
		// somebody deleted the port
		return ERR_PORT_DELETED;
	}

	if (res == ERR_SEM_TIMED_OUT) {
		// timed out, or, if timeout=0, 'would block'
		return ERR_PORT_TIMED_OUT;
	}

	if (res != NO_ERROR) {
		dprintf("write_port_etc: res unknown error %d\n", res);
		return res;
	}

	if (buffer_size > 0) {
		msg_store = cbuf_get_chain(buffer_size);
		if (msg_store == NULL)
			return ERR_NO_MEMORY;
		if (flags & PORT_FLAG_USE_USER_MEMCPY) {
			// copy from user memory
			if ((err = cbuf_user_memcpy_to_chain(msg_store, 0, msg_buffer, buffer_size)) < 0)
				return err; // memory exception
		} else
			// copy from kernel memory
			if ((err = cbuf_memcpy_to_chain(msg_store, 0, msg_buffer, buffer_size)) < 0)
				return err; // memory exception
	} else {
		msg_store = NULL;
	}

	// attach copied message to queue
	int_disable_interrupts();
	GRAB_PORT_LOCK(ports[slot]);

	h = ports[slot].head;
	if (h < 0)
		panic("port %id: head < 0", ports[slot].id);
	if (h >= ports[slot].capacity)
		panic("port %id: head > cap %d", ports[slot].id, ports[slot].capacity);
	ports[slot].msg_queue[h].msg_code	= msg_code;
	ports[slot].msg_queue[h].data_cbuf	= msg_store;
	ports[slot].msg_queue[h].data_len	= buffer_size;
	ports[slot].head = (ports[slot].head + 1) % ports[slot].capacity;
	ports[slot].total_count++;

	// store sem_id in local variable
	cached_semid = ports[slot].read_sem;

	RELEASE_PORT_LOCK(ports[slot]);
	int_restore_interrupts();

	sem_get_count(ports[slot].read_sem, &c1);
	sem_get_count(ports[slot].write_sem, &c2);

	// release sem, allowing read (might reschedule)
	sem_release(cached_semid, 1);

	return NO_ERROR;
}

/* this function cycles through the ports table, deleting all the ports that are owned by
   the passed proc_id */
int port_delete_owned_ports(proc_id owner)
{
	int i;
	int count = 0;

	if(ports_active == false)
		return ERR_PORT_NOT_ACTIVE;

	int_disable_interrupts();
	GRAB_PORT_LIST_LOCK();

	for(i=0; i<MAX_PORTS; i++) {
		if(ports[i].id != -1 && ports[i].owner == owner) {
			port_id id = ports[i].id;

			RELEASE_PORT_LIST_LOCK();
			int_restore_interrupts();

			port_delete(id);
			count++;

			int_disable_interrupts();
			GRAB_PORT_LIST_LOCK();
		}
	}

	RELEASE_PORT_LIST_LOCK();
	int_restore_interrupts();

	return count;
}


/*
 * testcode
 */

port_id test_p1, test_p2, test_p3, test_p4;

void port_test()
{
	char testdata[5];
	thread_id t;
	int res;
	int32 dummy;
	int32 dummy2;

	strcpy(testdata, "abcd");

	dprintf("porttest: port_create()\n");
	test_p1 = port_create(1,    "test port #1");
	test_p2 = port_create(10,   "test port #2");
	test_p3 = port_create(1024, "test port #3");
	test_p4 = port_create(1024, "test port #4");

	dprintf("porttest: port_find()\n");
	dprintf("'test port #1' has id %d (should be %d)\n", port_find("test port #1"), test_p1);

	dprintf("porttest: port_write() on 1, 2 and 3\n");
	port_write(test_p1, 1, &testdata, sizeof(testdata));
	port_write(test_p2, 666, &testdata, sizeof(testdata));
	port_write(test_p3, 999, &testdata, sizeof(testdata));
	dprintf("porttest: port_count(test_p1) = %d\n", port_count(test_p1));

	dprintf("porttest: port_write() on 1 with timeout of 1 sec (blocks 1 sec)\n");
	port_write_etc(test_p1, 1, &testdata, sizeof(testdata), PORT_FLAG_TIMEOUT, 1000000);
	dprintf("porttest: port_write() on 2 with timeout of 1 sec (wont block)\n");
	res = port_write_etc(test_p2, 777, &testdata, sizeof(testdata), PORT_FLAG_TIMEOUT, 1000000);
	dprintf("porttest: res=%d, %s\n", res, res == 0 ? "ok" : "BAD");

	dprintf("porttest: port_read() on empty port 4 with timeout of 1 sec (blocks 1 sec)\n");
	res = port_read_etc(test_p4, &dummy, &dummy2, sizeof(dummy2), PORT_FLAG_TIMEOUT, 1000000);
	dprintf("porttest: res=%d, %s\n", res, res == ERR_PORT_TIMED_OUT ? "ok" : "BAD");

	dprintf("porttest: spawning thread for port 1\n");
	t = thread_create_kernel_thread("port_test", port_test_thread_func, NULL);
	// resume thread
	thread_resume_thread(t);

	dprintf("porttest: write\n");
	port_write(test_p1, 1, &testdata, sizeof(testdata));

	// now we can write more (no blocking)
	dprintf("porttest: write #2\n");
	port_write(test_p1, 2, &testdata, sizeof(testdata));
	dprintf("porttest: write #3\n");
	port_write(test_p1, 3, &testdata, sizeof(testdata));

	dprintf("porttest: waiting on spawned thread\n");
	thread_wait_on_thread(t, NULL);

	dprintf("porttest: close p1\n");
	port_close(test_p2);
	dprintf("porttest: attempt write p1 after close\n");
	res = port_write(test_p2, 4, &testdata, sizeof(testdata));
	dprintf("porttest: port_write ret %d\n", res);

	dprintf("porttest: testing delete p2\n");
	port_delete(test_p2);

	dprintf("porttest: end test main thread\n");

}

int port_test_thread_func(void* arg)
{
	int msg_code;
	int n;
	char buf[6];
	buf[5] = '\0';

	dprintf("porttest: port_test_thread_func()\n");

	n = port_read(test_p1, &msg_code, &buf, 3);
	dprintf("port_read #1 code %d len %d buf %s\n", msg_code, n, buf);
	n = port_read(test_p1, &msg_code, &buf, 4);
	dprintf("port_read #1 code %d len %d buf %s\n", msg_code, n, buf);
	buf[4] = 'X';
	n = port_read(test_p1, &msg_code, &buf, 5);
	dprintf("port_read #1 code %d len %d buf %s\n", msg_code, n, buf);

	dprintf("porttest: testing delete p1 from other thread\n");
	port_delete(test_p1);
	dprintf("porttest: end port_test_thread_func()\n");

	return 0;
}

/*
 *	user level ports
 */

port_id user_port_create(int32 queue_length, const char *uname)
{
	dprintf("user_port_create: queue_length %d\n", queue_length);
	if(uname != NULL) {
		char name[SYS_MAX_OS_NAME_LEN];
		int rc;

		if(is_kernel_address(uname))
			return ERR_VM_BAD_USER_MEMORY;

		rc = user_strncpy(name, uname, SYS_MAX_OS_NAME_LEN-1);
		if(rc < 0)
			return rc;
		name[SYS_MAX_OS_NAME_LEN-1] = 0;

		return port_create(queue_length, name);
	} else {
		return port_create(queue_length, NULL);
	}
}

int	user_port_close(port_id id)
{
	return port_close(id);
}

int	user_port_delete(port_id id)
{
	return port_delete(id);
}

port_id	user_port_find(const char *port_name)
{
	if(port_name != NULL) {
		char name[SYS_MAX_OS_NAME_LEN];
		int rc;

		if(is_kernel_address(port_name))
			return ERR_VM_BAD_USER_MEMORY;

		rc = user_strncpy(name, port_name, SYS_MAX_OS_NAME_LEN-1);
		if(rc < 0)
			return rc;
		name[SYS_MAX_OS_NAME_LEN-1] = 0;

		return port_find(name);
	} else {
		return ERR_INVALID_ARGS;
	}
}

int	user_port_get_info(port_id id, struct port_info *uinfo)
{
	int 				res;
	struct port_info	info;
	int					rc;

	if (uinfo == NULL)
		return ERR_INVALID_ARGS;
	if(is_kernel_address(uinfo))
		return ERR_VM_BAD_USER_MEMORY;

	res = port_get_info(id, &info);
	// copy to userspace
	rc = user_memcpy(uinfo, &info, sizeof(struct port_info));
	if(rc < 0)
		return rc;
	return res;
}

int	user_port_get_next_port_info(proc_id uproc,
				uint32 *ucookie,
				struct port_info *uinfo)
{
	int 				res;
	struct port_info	info;
	uint32				cookie;
	int					rc;

	if (ucookie == NULL)
		return ERR_INVALID_ARGS;
	if (uinfo == NULL)
		return ERR_INVALID_ARGS;
	if(is_kernel_address(ucookie))
		return ERR_VM_BAD_USER_MEMORY;
	if(is_kernel_address(uinfo))
		return ERR_VM_BAD_USER_MEMORY;

	// copy from userspace
	rc = user_memcpy(&cookie, ucookie, sizeof(uint32));
	if(rc < 0)
		return rc;

	res = port_get_next_port_info(uproc, &cookie, &info);
	// copy to userspace
	rc = user_memcpy(ucookie, &info, sizeof(uint32));
	if(rc < 0)
		return rc;
	rc = user_memcpy(uinfo,   &info, sizeof(struct port_info));
	if(rc < 0)
		return rc;
	return res;
}

ssize_t user_port_buffer_size(port_id port)
{
	return port_buffer_size_etc(port, SEM_FLAG_INTERRUPTABLE, 0);
}

ssize_t	user_port_buffer_size_etc(port_id port, uint32 flags, bigtime_t timeout)
{
	return port_buffer_size_etc(port, flags | SEM_FLAG_INTERRUPTABLE, timeout);
}

int32 user_port_count(port_id port)
{
	return port_count(port);
}

ssize_t user_port_read(port_id uport, int32 *umsg_code, void *umsg_buffer,
							size_t ubuffer_size)
{
	return user_port_read_etc(uport, umsg_code, umsg_buffer, ubuffer_size, 0, 0);
}

ssize_t	user_port_read_etc(port_id uport, int32 *umsg_code, void *umsg_buffer,
							size_t ubuffer_size, uint32 uflags, bigtime_t utimeout)
{
	ssize_t	res;
	int32	msg_code;
	int		rc;

	if (umsg_code == NULL)
		return ERR_INVALID_ARGS;
	if (umsg_buffer == NULL)
		return ERR_INVALID_ARGS;

	if(is_kernel_address(umsg_code))
		return ERR_VM_BAD_USER_MEMORY;
	if(is_kernel_address(umsg_buffer))
		return ERR_VM_BAD_USER_MEMORY;

	res = port_read_etc(uport, &msg_code, umsg_buffer, ubuffer_size,
						uflags | PORT_FLAG_USE_USER_MEMCPY | SEM_FLAG_INTERRUPTABLE, utimeout);

	rc = user_memcpy(umsg_code, &msg_code, sizeof(int32));
	if(rc < 0)
		return rc;

	return res;
}

int	user_port_set_owner(port_id port, proc_id proc)
{
	return port_set_owner(port, proc);
}

int	user_port_write(port_id uport, int32 umsg_code, void *umsg_buffer,
				size_t ubuffer_size)
{
	return user_port_write_etc(uport, umsg_code, umsg_buffer, ubuffer_size, 0, 0);
}

int	user_port_write_etc(port_id uport, int32 umsg_code, void *umsg_buffer,
				size_t ubuffer_size, uint32 uflags, bigtime_t utimeout)
{
	if (umsg_buffer == NULL)
		return ERR_INVALID_ARGS;
	if(is_kernel_address(umsg_buffer))
		return ERR_VM_BAD_USER_MEMORY;
	return port_write_etc(uport, umsg_code, umsg_buffer, ubuffer_size,
		uflags | PORT_FLAG_USE_USER_MEMCPY | SEM_FLAG_INTERRUPTABLE, utimeout);
}

