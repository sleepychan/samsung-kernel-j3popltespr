/* Copyright (c) 2013-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/subsystem_notif.h>
#include <soc/qcom/msm_qmi_interface.h>
#include <soc/qcom/scm.h>
#include "msm_memshare.h"
#include "heap_mem_ext_v01.h"
#ifdef CONFIG_SEC_BSP
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <soc/qcom/secure_buffer.h>
#endif

#include <soc/qcom/secure_buffer.h>

/* Macros */
#define MEMSHARE_DEV_NAME "memshare"
#define MEMSHARE_CHILD_DEV_NAME "memshare_child"
static DEFINE_DMA_ATTRS(attrs);

static struct qmi_handle *mem_share_svc_handle;
static void mem_share_svc_recv_msg(struct work_struct *work);
static DECLARE_DELAYED_WORK(work_recv_msg, mem_share_svc_recv_msg);
static struct workqueue_struct *mem_share_svc_workqueue;
static uint64_t bootup_request;

/* Memshare Driver Structure */
struct memshare_driver {
	struct device *dev;
	struct mutex mem_share;
	struct mutex mem_free;
	struct work_struct memshare_init_work;
#ifdef CONFIG_SEC_BSP
	struct memshare_rd_device *memshare_rd_dev;
#endif
};

struct memshare_child {
	struct device *dev;
};

static struct memshare_driver *memsh_drv;
static struct memshare_child *memsh_child;
static struct mem_blocks memblock[MAX_CLIENTS];
static uint32_t num_clients;
static struct msg_desc mem_share_svc_alloc_req_desc = {
	.max_msg_len = MEM_ALLOC_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_ALLOC_REQ_MSG_V01,
	.ei_array = mem_alloc_req_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_alloc_resp_desc = {
	.max_msg_len = MEM_ALLOC_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_ALLOC_RESP_MSG_V01,
	.ei_array = mem_alloc_resp_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_free_req_desc = {
	.max_msg_len = MEM_FREE_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_FREE_REQ_MSG_V01,
	.ei_array = mem_free_req_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_free_resp_desc = {
	.max_msg_len = MEM_FREE_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_FREE_RESP_MSG_V01,
	.ei_array = mem_free_resp_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_alloc_generic_req_desc = {
	.max_msg_len = MEM_ALLOC_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_ALLOC_GENERIC_REQ_MSG_V01,
	.ei_array = mem_alloc_generic_req_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_alloc_generic_resp_desc = {
	.max_msg_len = MEM_ALLOC_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_ALLOC_GENERIC_RESP_MSG_V01,
	.ei_array = mem_alloc_generic_resp_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_free_generic_req_desc = {
	.max_msg_len = MEM_FREE_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_FREE_GENERIC_REQ_MSG_V01,
	.ei_array = mem_free_generic_req_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_free_generic_resp_desc = {
	.max_msg_len = MEM_FREE_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_FREE_GENERIC_RESP_MSG_V01,
	.ei_array = mem_free_generic_resp_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_size_query_req_desc = {
	.max_msg_len = MEM_FREE_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_QUERY_SIZE_REQ_MSG_V01,
	.ei_array = mem_query_size_req_msg_data_v01_ei,
};

static struct msg_desc mem_share_svc_size_query_resp_desc = {
	.max_msg_len = MEM_FREE_REQ_MAX_MSG_LEN_V01,
	.msg_id = MEM_QUERY_SIZE_RESP_MSG_V01,
	.ei_array = mem_query_size_resp_msg_data_v01_ei,
};

#ifdef CONFIG_SEC_BSP
struct memshare_rd_device {
	char name[256];
	struct miscdevice device;
	unsigned long address;
	void *v_address;
	unsigned long size;
	unsigned int data_ready;
	struct dma_attrs attrs;
};

static void memshare_set_nhlos_permission(phys_addr_t addr, u64 size)
{
	int ret;
	u32 source_vmlist[1] = {VMID_HLOS};
	int dest_vmids[2] = {VMID_MSS_MSA, VMID_HLOS};
	int dest_perms[2] = {PERM_READ|PERM_WRITE, PERM_READ};

	if (!size || !addr) {
		pr_err("%s: Unable to handle addr(0x%llx), size(%llu)",
				__func__, (uint64_t)addr, size);
		return;
	}

	ret = hyp_assign_phys(addr, size, source_vmlist, 1, dest_vmids,
				dest_perms, 2);

	pr_info("%s: hyp_assign_phys called addr(0x%llx) size(%llu)  ret:%d\n",
			__func__, (uint64_t)addr, size, ret);

	if (ret != 0) {
		if (ret == -ENOSYS)
			pr_warn("hyp_assign_phys is not supported!");
		else
			pr_err("hyp_assign_phys failed IPA=0x016%pa size=%llu err=%d\n",
				&addr, size, ret);
	}
}

static void memshare_unset_nhlos_permission(phys_addr_t addr, u64 size)
{
	int ret;
	u32 source_vmlist[2] = {VMID_MSS_MSA, VMID_HLOS};
	int dest_vmids[1] = {VMID_HLOS};
	int dest_perms[1] = {PERM_READ|PERM_WRITE|PERM_EXEC};

	if (!size || !addr) {
		pr_err("%s: Unable to handle addr(0x%llx), size(%llu)",
				__func__, (uint64_t)addr, size);
		return;
	}

	ret = hyp_assign_phys(addr, size, source_vmlist, 2, dest_vmids,
			dest_perms, 1);

	pr_info("%s: hyp_assign_phys called addr(0x%llx) size(%llu)  ret:%d\n",
			__func__, (uint64_t)addr, size, ret);

	if (ret != 0) {
		if (ret == -ENOSYS)
			pr_warn("hyp_assign_phys is not supported!");
		else
			pr_err("hyp_assign_phys failed IPA=0x016%pa size=%llu err=%d\n",
				&addr, size, ret);
	}
}

static int memshare_rd_open(struct inode *inode, struct file *filep)
{
	return 0;
}

static int memshare_rd_release(struct inode *inode, struct file *filep)
{
	return 0;
}

static ssize_t memshare_rd_read(struct file *filep, char __user *buf,
				size_t count, loff_t *pos)
{
	struct memshare_rd_device *rd_dev = container_of(filep->private_data,
				struct memshare_rd_device, device);
	void *device_mem;
	unsigned long data_left;
	unsigned long addr;
	unsigned long copy_size;
	int ret = 0;

	if ((filep->f_flags & O_NONBLOCK) && !rd_dev->data_ready)
		return -EAGAIN;

	data_left = rd_dev->size - *pos;
	addr = rd_dev->address + *pos;

	/* EOF check */
	if (data_left == 0) {
		pr_info("%s(%s): Ramdump complete. %zu bytes read.", __func__,
			rd_dev->name, (size_t)*pos);
		ret = 0;
		goto ramdump_done;
	}

#ifdef __aarch64__
	copy_size = min(count, (unsigned long)SZ_1M);
#else
	copy_size = min(count, (unsigned int)SZ_1M);
#endif
	copy_size = min(copy_size, data_left);

	device_mem = ioremap_nocache(addr, copy_size);

	if (device_mem == NULL) {
		pr_err("%s(%s): Unable to ioremap: addr %lx, copy_size %lu\n", __func__,
			rd_dev->name, addr, copy_size);
		ret = -ENOMEM;
		goto ramdump_done;
	}

	pr_debug("%s:copy_to_user(buf:%p, p_addr:%lx, device_mem :%p, copy_size :%lu\n",
			__func__, buf, addr, device_mem, copy_size);

	if (copy_to_user(buf, device_mem, copy_size)) {
		pr_err("%s(%s): Couldn't copy all data to user.", __func__,
			rd_dev->name);
		iounmap(device_mem);
		ret = -EFAULT;
		goto ramdump_done;
	}

	iounmap(device_mem);
	*pos += copy_size;

	pr_debug("%s(%s): Read %lu bytes from address %lx.", __func__,
			rd_dev->name, copy_size, addr);

	return copy_size;

ramdump_done:
	*pos = 0;
	return ret;
}

static const struct file_operations memshare_rd_file_ops = {
	.open = memshare_rd_open,
	.release = memshare_rd_release,
	.read = memshare_rd_read
};

static void *create_memshare_rd_device(const char *dev_name,
		struct device *parent)
{
	int ret;
	struct memshare_rd_device *rd_dev;

	if (!dev_name) {
		pr_err("%s: Invalid device name.\n", __func__);
		return NULL;
	}

	rd_dev = kzalloc(sizeof(struct memshare_rd_device), GFP_KERNEL);
	if (!rd_dev) {
		pr_err("%s: Couldn't alloc space for ramdump device!",
				__func__);
		return NULL;
	}

	snprintf(rd_dev->name, ARRAY_SIZE(rd_dev->name), "ramdump_%s",
			dev_name);

	rd_dev->device.minor = MISC_DYNAMIC_MINOR;
	rd_dev->device.name = rd_dev->name;
	rd_dev->device.fops = &memshare_rd_file_ops;
	rd_dev->device.parent = parent;

	ret = misc_register(&rd_dev->device);

	if (ret) {
		pr_err("%s: misc_register failed for %s (%d)", __func__,
				dev_name, ret);
		kfree(rd_dev);
		return NULL;
	}

	return (void *)rd_dev;
}

static void destroy_memshare_rd_device(void *dev)
{
	struct memshare_rd_device *rd_dev = dev;

	if (IS_ERR_OR_NULL(rd_dev))
		return ;

	misc_deregister(&rd_dev->device);
	kfree(rd_dev);
}

static int memshare_rd_set(struct memshare_rd_device *rd_dev,
		phys_addr_t p_addr, unsigned long size, void *v_addr)
{
	int rc = 0;

	rd_dev->address = p_addr;
	rd_dev->v_address = v_addr;
	rd_dev->size = size;
	rd_dev->data_ready = 1;

	pr_debug("%s: p_addr(%llx), size(%lu)\n", __func__, (uint64_t)p_addr, size);

	return rc;
}
#endif

static int check_client(int client_id, int proc, int request)
{
	int i = 0;
	int found = DHMS_MEM_CLIENT_INVALID;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (memblock[i].client_id == client_id &&
				memblock[i].peripheral == proc) {
			found = i;
			break;
		}
	}
	if ((found == DHMS_MEM_CLIENT_INVALID) && !request) {
		pr_debug("No registered client, adding a new client\n");
		/* Add a new client */
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (memblock[i].client_id == DHMS_MEM_CLIENT_INVALID) {
				memblock[i].client_id = client_id;
				memblock[i].alloted = 0;
				memblock[i].guarantee = 0;
				memblock[i].peripheral = proc;
				found = i;
				break;
			}
		}
	}

	return found;
}

void free_client(int id)
{
	memblock[id].size = 0;
	memblock[id].phy_addr = 0;
	memblock[id].virtual_addr = 0;
	memblock[id].alloted = 0;
	memblock[id].client_id = DHMS_MEM_CLIENT_INVALID;
	memblock[id].guarantee = 0;
	memblock[id].peripheral = -1;
	memblock[id].sequence_id = -1;
	memblock[id].memory_type = MEMORY_CMA;

}

void free_mem_clients(int proc)
{
	size_t i;

	pr_debug("memshare: freeing clients\n");

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (memblock[i].peripheral == proc &&
				!memblock[i].guarantee && memblock[i].alloted) {
			pr_debug("Freeing memory for client id: %d\n",
					memblock[i].client_id);
			dma_free_attrs(memsh_drv->dev, memblock[i].size,
				memblock[i].virtual_addr, memblock[i].phy_addr,
				&attrs);
			free_client(i);
		}
	}
}

void fill_alloc_response(struct mem_alloc_generic_resp_msg_v01 *resp,
						int id, int *flag)
{
	resp->sequence_id_valid = 1;
	resp->sequence_id = memblock[id].sequence_id;
	resp->dhms_mem_alloc_addr_info_valid = 1;
	resp->dhms_mem_alloc_addr_info_len = 1;
	resp->dhms_mem_alloc_addr_info[0].phy_addr = memblock[id].phy_addr;
	resp->dhms_mem_alloc_addr_info[0].num_bytes = memblock[id].size;
	if (!*flag) {
		resp->resp.result = QMI_RESULT_SUCCESS_V01;
		resp->resp.error = QMI_ERR_NONE_V01;
	} else {
		resp->resp.result = QMI_RESULT_FAILURE_V01;
		resp->resp.error = QMI_ERR_NO_MEMORY_V01;
	}

}

void initialize_client(void)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		memblock[i].alloted = 0;
		memblock[i].size = 0;
		memblock[i].guarantee = 0;
		memblock[i].phy_addr = 0;
		memblock[i].virtual_addr = 0;
		memblock[i].client_id = DHMS_MEM_CLIENT_INVALID;
		memblock[i].peripheral = -1;
		memblock[i].sequence_id = -1;
		memblock[i].memory_type = MEMORY_CMA;
		memblock[i].free_memory = 0;
		memblock[i].hyp_mapping = 0;
	}
#ifndef CONFIG_SEC_BSP
	dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
#endif
}

static int modem_notifier_cb(struct notifier_block *this, unsigned long code,
					void *_cmd)
{
	int i;
	int ret;
	u32 source_vmlist[2] = {VMID_HLOS, VMID_MSS_MSA};
	int dest_vmids[1] = {VMID_HLOS};
	int dest_perms[1] = {PERM_READ|PERM_WRITE};

	mutex_lock(&memsh_drv->mem_share);
	switch (code) {

	case SUBSYS_BEFORE_SHUTDOWN:
		bootup_request++;
		break;

	case SUBSYS_AFTER_POWERUP:
		pr_debug("memshare: Modem has booted up\n");
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (memblock[i].free_memory > 0 &&
					bootup_request >= 2) {
				memblock[i].free_memory -= 1;
				pr_debug("memshare: free_memory count: %d for clinet id: %d\n",
					memblock[i].free_memory,
					memblock[i].client_id);
			}

			if (memblock[i].free_memory == 0) {
				if (memblock[i].peripheral ==
					DHMS_MEM_PROC_MPSS_V01 &&
					!memblock[i].guarantee &&
					memblock[i].alloted) {
					pr_debug("memshare: Freeing memory for client id: %d\n",
						memblock[i].client_id);
					ret = hyp_assign_phys(
							memblock[i].phy_addr,
							memblock[i].size,
							source_vmlist,
							2, dest_vmids,
							dest_perms, 1);
					if (ret &&
						memblock[i].hyp_mapping == 1) {
						/*
						 * This is an error case as hyp
						 * mapping was successful
						 * earlier but during unmap
						 * it lead to failure.
						 */
						pr_err("memshare: %s, failed to unmap the region\n",
							__func__);
						memblock[i].hyp_mapping = 1;
					} else {
						memblock[i].hyp_mapping = 0;
					}
					dma_free_attrs(memsh_drv->dev,
						memblock[i].size,
						memblock[i].virtual_addr,
						memblock[i].phy_addr,
						&attrs);
					free_client(i);
				}
			}
		}
		bootup_request++;
		break;

#ifdef CONFIG_SEC_BSP
	case SUBSYS_AFTER_SHUTDOWN:
		pr_err("memshare: Modem shutdown has happened\n");
		memshare_unset_nhlos_permission(memsh_drv->memshare_rd_dev->address,
				memsh_drv->memshare_rd_dev->size);
		memsh_drv->memshare_rd_dev->data_ready = 0;
		free_mem_clients(DHMS_MEM_PROC_MPSS_V01);
		break;
#endif
	default:
		pr_debug("Memshare: code: %lu\n", code);
		break;
	}

	mutex_unlock(&memsh_drv->mem_share);
	return NOTIFY_DONE;
}

static struct notifier_block nb = {
	.notifier_call = modem_notifier_cb,
};

static void shared_hyp_mapping(int client_id)
{
	int ret;
	u32 source_vmlist[1] = {VMID_HLOS};
	int dest_vmids[2] = {VMID_HLOS, VMID_MSS_MSA};
	int dest_perms[2] = {PERM_READ|PERM_WRITE,
				PERM_READ|PERM_WRITE};

	if (client_id == DHMS_MEM_CLIENT_INVALID) {
		pr_err("memshare: %s, Invalid Client\n", __func__);
		return;
	}

	ret = hyp_assign_phys(memblock[client_id].phy_addr,
			memblock[client_id].size,
			source_vmlist, 1, dest_vmids,
			dest_perms, 2);

	if (ret != 0) {
		pr_err("memshare: hyp_assign_phys failed size=%u err=%d\n",
				memblock[client_id].size, ret);
		return;
	}
	memblock[client_id].hyp_mapping = 1;
}

static int handle_alloc_req(void *req_h, void *req, void *conn_h)
{
	struct mem_alloc_req_msg_v01 *alloc_req;
	struct mem_alloc_resp_msg_v01 alloc_resp;
	int rc = 0;

	alloc_req = (struct mem_alloc_req_msg_v01 *)req;
	pr_debug("%s: Received Alloc Request\n", __func__);
	pr_debug("%s: req->num_bytes = %d\n", __func__, alloc_req->num_bytes);
	mutex_lock(&memsh_drv->mem_share);
	if (!memblock[GPS].size) {
		memset(&alloc_resp, 0, sizeof(struct mem_alloc_resp_msg_v01));
		alloc_resp.resp = QMI_RESULT_FAILURE_V01;
		rc = memshare_alloc(memsh_drv->dev, alloc_req->num_bytes,
					&memblock[GPS]);
	}
	alloc_resp.num_bytes_valid = 1;
	alloc_resp.num_bytes =  alloc_req->num_bytes;
	alloc_resp.handle_valid = 1;
	alloc_resp.handle = memblock[GPS].phy_addr;
	if (rc) {
		alloc_resp.resp = QMI_RESULT_FAILURE_V01;
		memblock[GPS].size = 0;
	} else {
#ifdef CONFIG_SEC_BSP
		memshare_set_nhlos_permission(memblock[GPS].phy_addr,
				alloc_req->num_bytes);
		memshare_rd_set(memsh_drv->memshare_rd_dev, memblock[GPS].phy_addr,
				alloc_req->num_bytes, memblock[GPS].virtual_addr);
		memblock[GPS].alloted = 1;
		memblock[GPS].size = alloc_req->num_bytes;
#endif
		alloc_resp.resp = QMI_RESULT_SUCCESS_V01;
	}

	mutex_unlock(&memsh_drv->mem_share);

	pr_debug("alloc_resp.num_bytes :%d, alloc_resp.handle :%lx, alloc_resp.mem_req_result :%lx\n",
			  alloc_resp.num_bytes,
			  (unsigned long int)alloc_resp.handle,
			  (unsigned long int)alloc_resp.resp);

	rc = qmi_send_resp_from_cb(mem_share_svc_handle, conn_h, req_h,
			&mem_share_svc_alloc_resp_desc, &alloc_resp,
			sizeof(alloc_resp));
	if (rc < 0)
		pr_err("In %s, Error sending the alloc request: %d\n",
					__func__, rc);

	return rc;
}

static int handle_alloc_generic_req(void *req_h, void *req, void *conn_h)
{
	struct mem_alloc_generic_req_msg_v01 *alloc_req;
	struct mem_alloc_generic_resp_msg_v01 *alloc_resp;
	int rc, resp = 0;
	int client_id;

	alloc_req = (struct mem_alloc_generic_req_msg_v01 *)req;
	pr_debug("memshare: alloc request client id: %d proc _id: %d\n",
			alloc_req->client_id, alloc_req->proc_id);
	mutex_lock(&memsh_drv->mem_share);
	alloc_resp = kzalloc(sizeof(struct mem_alloc_generic_resp_msg_v01),
					GFP_KERNEL);
	if (!alloc_resp) {
		mutex_unlock(&memsh_drv->mem_share);
		return -ENOMEM;
	}
	alloc_resp->resp.result = QMI_RESULT_FAILURE_V01;
	alloc_resp->resp.error = QMI_ERR_NO_MEMORY_V01;
	client_id = check_client(alloc_req->client_id, alloc_req->proc_id,
								CHECK);

	if (client_id >= MAX_CLIENTS) {
		pr_err("memshare: %s client not found, requested client: %d, proc_id: %d\n",
				__func__, alloc_req->client_id,
				alloc_req->proc_id);
		return -EINVAL;
	}

	memblock[client_id].free_memory += 1;
	pr_debug("memshare: In %s, free memory count for client id: %d = %d",
		__func__, memblock[client_id].client_id,
			memblock[client_id].free_memory);
	if (!memblock[client_id].alloted) {
		rc = memshare_alloc(memsh_drv->dev, alloc_req->num_bytes,
					&memblock[client_id]);
		if (rc) {
			pr_err("In %s,Unable to allocate memory for requested client\n",
							__func__);
			resp = 1;
		}
		if (!resp) {
			memblock[client_id].alloted = 1;
			memblock[client_id].size = alloc_req->num_bytes;
			memblock[client_id].peripheral = alloc_req->proc_id;
#ifdef CONFIG_SEC_BSP
			if (VENDOR == memblock[client_id].client_id &&
			    DHMS_MEM_PROC_MPSS_V01 == memblock[client_id].peripheral) {
				memshare_set_nhlos_permission(memblock[client_id].phy_addr,
						alloc_req->num_bytes);
				memshare_rd_set(memsh_drv->memshare_rd_dev,
						memblock[client_id].phy_addr,
						alloc_req->num_bytes,
						memblock[client_id].virtual_addr);
			}
#endif
		}
	}
	memblock[client_id].sequence_id = alloc_req->sequence_id;

	fill_alloc_response(alloc_resp, client_id, &resp);
	/*
	 * Perform the Hypervisor mapping in order to avoid XPU viloation
	 * to the allocated region for Modem Clients
	 */
	if (!memblock[client_id].hyp_mapping &&
		memblock[client_id].alloted)
		shared_hyp_mapping(client_id);
	mutex_unlock(&memsh_drv->mem_share);
	pr_debug("memshare: alloc_resp.num_bytes :%d, alloc_resp.handle :%lx, alloc_resp.mem_req_result :%lx\n",
			  alloc_resp->dhms_mem_alloc_addr_info[0].num_bytes,
			  (unsigned long int)
			  alloc_resp->dhms_mem_alloc_addr_info[0].phy_addr,
			  (unsigned long int)alloc_resp->resp.result);
	rc = qmi_send_resp_from_cb(mem_share_svc_handle, conn_h, req_h,
			&mem_share_svc_alloc_generic_resp_desc, alloc_resp,
			sizeof(alloc_resp));

	kfree(alloc_resp);

	if (rc < 0)
		pr_err("In %s, Error sending the alloc request: %d\n",
							__func__, rc);
	return rc;
}

static int handle_free_req(void *req_h, void *req, void *conn_h)
{
	struct mem_free_req_msg_v01 *free_req;
	struct mem_free_resp_msg_v01 free_resp;
	int rc;

	mutex_lock(&memsh_drv->mem_free);
	if (!memblock[GPS].guarantee) {
		free_req = (struct mem_free_req_msg_v01 *)req;
		pr_debug("%s: Received Free Request\n", __func__);
		memset(&free_resp, 0, sizeof(struct mem_free_resp_msg_v01));
		pr_debug("In %s: pblk->virtual_addr :%lx, pblk->phy_addr: %lx\n,size: %d",
				__func__,
			(unsigned long int)memblock[GPS].virtual_addr,
			(unsigned long int)free_req->handle,
			memblock[GPS].size);
		dma_free_coherent(memsh_drv->dev, memblock[GPS].size,
			memblock[GPS].virtual_addr,
				free_req->handle);
	}
#ifdef CONFIG_SEC_BSP
	memshare_unset_nhlos_permission(memblock[GPS].phy_addr,
			memsh_drv->memshare_rd_dev->size);
	memsh_drv->memshare_rd_dev->data_ready = 0;
#endif
	free_resp.resp = QMI_RESULT_SUCCESS_V01;
	mutex_unlock(&memsh_drv->mem_free);
	rc = qmi_send_resp_from_cb(mem_share_svc_handle, conn_h, req_h,
			&mem_share_svc_free_resp_desc, &free_resp,
			sizeof(free_resp));
	if (rc < 0)
		pr_err("In %s, Error sending the free request: %d\n",
					__func__, rc);

	return rc;
}

static int handle_free_generic_req(void *req_h, void *req, void *conn_h)
{
	struct mem_free_generic_req_msg_v01 *free_req;
	struct mem_free_generic_resp_msg_v01 free_resp;
	int rc;
	int flag = 0;
	uint32_t client_id;

	free_req = (struct mem_free_generic_req_msg_v01 *)req;
	pr_debug("memshare: %s: Received Free Request\n", __func__);
	mutex_lock(&memsh_drv->mem_free);
	memset(&free_resp, 0, sizeof(struct mem_free_generic_resp_msg_v01));
	free_resp.resp.error = QMI_ERR_INTERNAL_V01;
	free_resp.resp.result = QMI_RESULT_FAILURE_V01;
	pr_debug("memshare: Client id: %d proc id: %d\n", free_req->client_id,
				free_req->proc_id);
	client_id = check_client(free_req->client_id, free_req->proc_id, FREE);
	if (client_id == DHMS_MEM_CLIENT_INVALID) {
		pr_err("In %s, Invalid client request to free memory\n",
					__func__);
		flag = 1;
	} else if (!memblock[client_id].guarantee &&
					memblock[client_id].alloted) {
		pr_debug("In %s: pblk->virtual_addr :%lx, pblk->phy_addr: %lx\n,size: %d",
				__func__,
				(unsigned long int)
				memblock[client_id].virtual_addr,
				(unsigned long int)memblock[client_id].phy_addr,
				memblock[client_id].size);
		dma_free_attrs(memsh_drv->dev, memblock[client_id].size,
			memblock[client_id].virtual_addr,
			memblock[client_id].phy_addr,
			&attrs);
		free_client(client_id);
	} else {
		pr_err("In %s, Request came for a guaranteed client cannot free up the memory\n",
						__func__);
	}

#ifdef CONFIG_SEC_BSP
	if (VENDOR == memblock[client_id].client_id &&
	    DHMS_MEM_PROC_MPSS_V01 == memblock[client_id].peripheral) {
		memshare_unset_nhlos_permission(memblock[client_id].phy_addr,
				memsh_drv->memshare_rd_dev->size);
		memsh_drv->memshare_rd_dev->data_ready = 0;
	}
#endif

	if (flag) {
		free_resp.resp.result = QMI_RESULT_FAILURE_V01;
		free_resp.resp.error = QMI_ERR_INVALID_ID_V01;
	} else {
		free_resp.resp.result = QMI_RESULT_SUCCESS_V01;
		free_resp.resp.error = QMI_ERR_NONE_V01;
	}

	mutex_unlock(&memsh_drv->mem_free);
	rc = qmi_send_resp_from_cb(mem_share_svc_handle, conn_h, req_h,
		&mem_share_svc_free_generic_resp_desc, &free_resp,
		sizeof(free_resp));

	if (rc < 0)
		pr_err("In %s, Error sending the free request: %d\n",
					__func__, rc);

	return rc;
}

static int handle_query_size_req(void *req_h, void *req, void *conn_h)
{
	int rc, client_id;
	struct mem_query_size_req_msg_v01 *query_req;
	struct mem_query_size_rsp_msg_v01 *query_resp;

	query_req = (struct mem_query_size_req_msg_v01 *)req;
	mutex_lock(&memsh_drv->mem_share);
	query_resp = kzalloc(sizeof(struct mem_query_size_rsp_msg_v01),
					GFP_KERNEL);
	if (!query_resp) {
		mutex_unlock(&memsh_drv->mem_share);
		return -ENOMEM;
	}
	pr_debug("memshare: query request client id: %d proc _id: %d\n",
		query_req->client_id, query_req->proc_id);
	client_id = check_client(query_req->client_id, query_req->proc_id,
								CHECK);

	if (client_id >= MAX_CLIENTS) {
		pr_err("memshare: %s client not found, requested client: %d, proc_id: %d\n",
				__func__, query_req->client_id,
				query_req->proc_id);
		return -EINVAL;
	}

	if (memblock[client_id].size) {
		query_resp->size_valid = 1;
		query_resp->size = memblock[client_id].size;
	} else {
		query_resp->size_valid = 1;
		query_resp->size = 0;
	}
	query_resp->resp.result = QMI_RESULT_SUCCESS_V01;
	query_resp->resp.error = QMI_ERR_NONE_V01;
	mutex_unlock(&memsh_drv->mem_share);

	pr_debug("memshare: query_resp.size :%d, alloc_resp.mem_req_result :%lx\n",
			  query_resp->size,
			  (unsigned long int)query_resp->resp.result);
	rc = qmi_send_resp_from_cb(mem_share_svc_handle, conn_h, req_h,
			&mem_share_svc_size_query_resp_desc, query_resp,
			sizeof(query_resp));

	if (rc < 0)
		pr_err("In %s, Error sending the query request: %d\n",
							__func__, rc);

	return rc;
}

static int mem_share_svc_connect_cb(struct qmi_handle *handle,
			       void *conn_h)
{
	if (mem_share_svc_handle != handle || !conn_h)
		return -EINVAL;

	return 0;
}

static int mem_share_svc_disconnect_cb(struct qmi_handle *handle,
				  void *conn_h)
{
	if (mem_share_svc_handle != handle || !conn_h)
		return -EINVAL;

	return 0;
}

static int mem_share_svc_req_desc_cb(unsigned int msg_id,
				struct msg_desc **req_desc)
{
	int rc;

	pr_debug("memshare: In %s\n", __func__);
	switch (msg_id) {
	case MEM_ALLOC_REQ_MSG_V01:
		*req_desc = &mem_share_svc_alloc_req_desc;
		rc = sizeof(struct mem_alloc_req_msg_v01);
		break;

	case MEM_FREE_REQ_MSG_V01:
		*req_desc = &mem_share_svc_free_req_desc;
		rc = sizeof(struct mem_free_req_msg_v01);
		break;

	case MEM_ALLOC_GENERIC_REQ_MSG_V01:
		*req_desc = &mem_share_svc_alloc_generic_req_desc;
		rc = sizeof(struct mem_alloc_generic_req_msg_v01);
		break;

	case MEM_FREE_GENERIC_REQ_MSG_V01:
		*req_desc = &mem_share_svc_free_generic_req_desc;
		rc = sizeof(struct mem_free_generic_req_msg_v01);
		break;

	case MEM_QUERY_SIZE_REQ_MSG_V01:
		*req_desc = &mem_share_svc_size_query_req_desc;
		rc = sizeof(struct mem_query_size_req_msg_v01);
		break;

	default:
		rc = -ENOTSUPP;
		break;
	}
	return rc;
}

static int mem_share_svc_req_cb(struct qmi_handle *handle, void *conn_h,
			void *req_h, unsigned int msg_id, void *req)
{
	int rc;

	pr_debug("memshare: In %s\n", __func__);
	if (mem_share_svc_handle != handle || !conn_h)
		return -EINVAL;

	switch (msg_id) {
	case MEM_ALLOC_REQ_MSG_V01:
		rc = handle_alloc_req(req_h, req, conn_h);
		break;

	case MEM_FREE_REQ_MSG_V01:
		rc = handle_free_req(req_h, req, conn_h);
		break;

	case MEM_ALLOC_GENERIC_REQ_MSG_V01:
		rc = handle_alloc_generic_req(req_h, req, conn_h);
		break;

	case MEM_FREE_GENERIC_REQ_MSG_V01:
		rc = handle_free_generic_req(req_h, req, conn_h);
		break;

	case MEM_QUERY_SIZE_REQ_MSG_V01:
		rc = handle_query_size_req(req_h, req, conn_h);
		break;

	default:
		rc = -ENOTSUPP;
		break;
	}
	return rc;
}

static void mem_share_svc_recv_msg(struct work_struct *work)
{
	int rc;

	pr_debug("memshare: In %s\n", __func__);
	do {
		pr_debug("%s: Notified about a Receive Event", __func__);
	} while ((rc = qmi_recv_msg(mem_share_svc_handle)) == 0);

	if (rc != -ENOMSG)
		pr_err("%s: Error receiving message\n", __func__);
}

static void qmi_mem_share_svc_ntfy(struct qmi_handle *handle,
		enum qmi_event_type event, void *priv)
{
	pr_debug("memshare: In %s\n", __func__);
	switch (event) {
	case QMI_RECV_MSG:
		queue_delayed_work(mem_share_svc_workqueue,
				   &work_recv_msg, 0);
		break;
	default:
		break;
	}
}

static struct qmi_svc_ops_options mem_share_svc_ops_options = {
	.version = 1,
	.service_id = MEM_SHARE_SERVICE_SVC_ID,
	.service_vers = MEM_SHARE_SERVICE_VERS,
	.service_ins = MEM_SHARE_SERVICE_INS_ID,
	.connect_cb = mem_share_svc_connect_cb,
	.disconnect_cb = mem_share_svc_disconnect_cb,
	.req_desc_cb = mem_share_svc_req_desc_cb,
	.req_cb = mem_share_svc_req_cb,
};

int memshare_alloc(struct device *dev,
					unsigned int block_size,
					struct mem_blocks *pblk)
{

	int ret;

	pr_debug("%s: memshare_alloc called", __func__);
	if (!pblk) {
		pr_err("%s: Failed to alloc\n", __func__);
		return -ENOMEM;
	}

	pblk->virtual_addr = dma_alloc_attrs(dev, block_size,
						&pblk->phy_addr, GFP_KERNEL,
						&attrs);
	if (pblk->virtual_addr == NULL) {
		pr_err("allocation failed, %d\n", block_size);
		ret = -ENOMEM;
		return ret;
	}
	pr_debug("pblk->phy_addr :%lx, pblk->virtual_addr %lx\n",
		  (unsigned long int)pblk->phy_addr,
		  (unsigned long int)pblk->virtual_addr);
	return 0;
}

static void memshare_init_worker(struct work_struct *work)
{
	int rc;

	mem_share_svc_workqueue =
		create_singlethread_workqueue("mem_share_svc");
	if (!mem_share_svc_workqueue)
		return;

	mem_share_svc_handle = qmi_handle_create(qmi_mem_share_svc_ntfy, NULL);
	if (!mem_share_svc_handle) {
		pr_err("%s: Creating mem_share_svc qmi handle failed\n",
			__func__);
		destroy_workqueue(mem_share_svc_workqueue);
		return;
	}
	rc = qmi_svc_register(mem_share_svc_handle, &mem_share_svc_ops_options);
	if (rc < 0) {
		pr_err("%s: Registering mem share svc failed %d\n",
			__func__, rc);
		qmi_handle_destroy(mem_share_svc_handle);
		destroy_workqueue(mem_share_svc_workqueue);
		return;
	}
	pr_debug("memshare: memshare_init successful\n");
}

static int memshare_child_probe(struct platform_device *pdev)
{
	int rc;
	uint32_t size, client_id;
	const char *name;
	struct memshare_child *drv;

	drv = devm_kzalloc(&pdev->dev, sizeof(struct memshare_child),
							GFP_KERNEL);

	if (!drv) {
		pr_err("Unable to allocate memory to driver\n");
		return -ENOMEM;
	}

	drv->dev = &pdev->dev;
	memsh_child = drv;
	platform_set_drvdata(pdev, memsh_child);

	rc = of_property_read_u32(pdev->dev.of_node, "qcom,peripheral-size",
						&size);
	if (rc) {
		pr_err("In %s, Error reading size of clients, rc: %d\n",
				__func__, rc);
		return rc;
	}

	rc = of_property_read_u32(pdev->dev.of_node, "qcom,client-id",
						&client_id);
	if (rc) {
		pr_err("In %s, Error reading client id, rc: %d\n",
				__func__, rc);
		return rc;
	}

	memblock[num_clients].guarantee = of_property_read_bool(
							pdev->dev.of_node,
							"qcom,allocate-boot-time");

	rc = of_property_read_string(pdev->dev.of_node, "label",
						&name);
	if (rc) {
		pr_err("In %s, Error reading peripheral info for client, rc: %d\n",
					__func__, rc);
		return rc;
	}

	if (strcmp(name, "modem") == 0)
		memblock[num_clients].peripheral = DHMS_MEM_PROC_MPSS_V01;
	else if (strcmp(name, "adsp") == 0)
		memblock[num_clients].peripheral = DHMS_MEM_PROC_ADSP_V01;
	else if (strcmp(name, "wcnss") == 0)
		memblock[num_clients].peripheral = DHMS_MEM_PROC_WCNSS_V01;

	memblock[num_clients].size = size;
	memblock[num_clients].client_id = client_id;

	if (memblock[num_clients].guarantee) {
		rc = memshare_alloc(memsh_child->dev,
				memblock[num_clients].size,
				&memblock[num_clients]);
		if (rc) {
			pr_err("In %s, Unable to allocate memory for guaranteed clients, rc: %d\n",
							__func__, rc);
			return rc;
		}
		memblock[num_clients].alloted = 1;
	}

	num_clients++;

	return 0;
}

static int memshare_probe(struct platform_device *pdev)
{
	int rc;
	struct memshare_driver *drv;

	drv = devm_kzalloc(&pdev->dev, sizeof(struct memshare_driver),
							GFP_KERNEL);

	if (!drv) {
		pr_err("Unable to allocate memory to driver\n");
		return -ENOMEM;
	}

	/* Memory allocation has been done successfully */
	mutex_init(&drv->mem_free);
	mutex_init(&drv->mem_share);

	INIT_WORK(&drv->memshare_init_work, memshare_init_worker);
	schedule_work(&drv->memshare_init_work);

	drv->dev = &pdev->dev;
	memsh_drv = drv;
	platform_set_drvdata(pdev, memsh_drv);
	initialize_client();
	num_clients = 0;

	rc = of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);

	if (rc) {
		pr_err("In %s, error populating the devices\n", __func__);
		return rc;
	}

	subsys_notif_register_notifier("modem", &nb);
	pr_info("In %s, Memshare probe success\n", __func__);

#ifdef CONFIG_SEC_BSP
	drv->memshare_rd_dev = create_memshare_rd_device(MEMSHARE_DEV_NAME,
								&pdev->dev);
	if (!drv->memshare_rd_dev) {
		pr_err("%s : Unable to create a memshare ramdump device.\n",
				__func__);
		rc = -ENOMEM;
		return rc;
	}
#endif

	return 0;
}

static int memshare_remove(struct platform_device *pdev)
{
	if (!memsh_drv)
		return 0;

	qmi_svc_unregister(mem_share_svc_handle);
	flush_workqueue(mem_share_svc_workqueue);
	qmi_handle_destroy(mem_share_svc_handle);
	destroy_workqueue(mem_share_svc_workqueue);

#ifdef CONFIG_SEC_BSP
	destroy_memshare_rd_device(memsh_drv->memshare_rd_dev);
#endif
	return 0;
}

static int memshare_child_remove(struct platform_device *pdev)
{
	if (!memsh_child)
		return 0;

	return 0;
}

static struct of_device_id memshare_match_table[] = {
	{
		.compatible = "qcom,memshare",
	},
	{}
};

static struct of_device_id memshare_match_table1[] = {
	{
		.compatible = "qcom,memshare-peripheral",
	},
	{}
};


static struct platform_driver memshare_pdriver = {
	.probe          = memshare_probe,
	.remove         = memshare_remove,
	.driver = {
		.name   = MEMSHARE_DEV_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = memshare_match_table,
	},
};

static struct platform_driver memshare_pchild = {
	.probe          = memshare_child_probe,
	.remove         = memshare_child_remove,
	.driver = {
		.name   = MEMSHARE_CHILD_DEV_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = memshare_match_table1,
	},
};

module_platform_driver(memshare_pdriver);
module_platform_driver(memshare_pchild);

MODULE_DESCRIPTION("Mem Share QMI Service Driver");
MODULE_LICENSE("GPL v2");
