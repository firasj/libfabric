/*
 * Copyright (c) 2022 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "efa.h"

#if HAVE_CUDA || HAVE_NEURON
static size_t efa_max_eager_msg_size_with_largest_header(struct efa_domain *efa_domain) {
	int mtu_size;

	mtu_size = efa_domain->device->rdm_info->ep_attr->max_msg_size;
	if (rxr_env.mtu_size > 0 && rxr_env.mtu_size < mtu_size)
		mtu_size = rxr_env.mtu_size;
	if (mtu_size > RXR_MTU_MAX_LIMIT)
		mtu_size = RXR_MTU_MAX_LIMIT;

	return mtu_size - rxr_pkt_max_hdr_size();
}
#else
static size_t efa_max_eager_msg_size_with_largest_header(struct efa_domain *efa_domain) {
	return 0;
}
#endif

static int efa_hmem_info_init_protocol_thresholds(struct efa_hmem_info *info, enum fi_hmem_iface iface, struct efa_domain *efa_domain) {
	switch (iface) {
	case FI_HMEM_SYSTEM:
		/* We have not yet tested runting with system memory */
		info->runt_size = 0;
		info->max_medium_msg_size = EFA_DEFAULT_INTER_MAX_MEDIUM_MESSAGE_SIZE;
		info->min_read_msg_size = EFA_DEFAULT_INTER_MIN_READ_MESSAGE_SIZE;
		info->min_read_write_size = EFA_DEFAULT_INTER_MIN_READ_WRITE_SIZE;
		fi_param_get_size_t(&rxr_prov, "runt_size", &info->runt_size);
		fi_param_get_size_t(&rxr_prov, "inter_max_medium_message_size", &info->max_medium_msg_size);
		fi_param_get_size_t(&rxr_prov, "inter_min_read_message_size", &info->min_read_msg_size);
		fi_param_get_size_t(&rxr_prov, "inter_min_read_write_size", &info->min_read_write_size);
		break;
	case FI_HMEM_CUDA:
	case FI_HMEM_NEURON:
		info->runt_size = EFA_DEFAULT_RUNT_SIZE;
		info->max_medium_msg_size = 0;
		info->min_read_msg_size = efa_max_eager_msg_size_with_largest_header(efa_domain) + 1;
		info->min_read_write_size = efa_max_eager_msg_size_with_largest_header(efa_domain) + 1;
		fi_param_get_size_t(&rxr_prov, "runt_size", &info->runt_size);
		fi_param_get_size_t(&rxr_prov, "inter_min_read_message_size", &info->min_read_msg_size);
		fi_param_get_size_t(&rxr_prov, "inter_min_read_write_size", &info->min_read_write_size);
		break;
	case FI_HMEM_SYNAPSEAI:
		info->runt_size = 0;
		info->max_medium_msg_size = 0;
		info->min_read_msg_size = 1;
		info->min_read_write_size = 1;
		break;
	default:
		break;
	}
	return 0;
}

/**
 * @brief Initialize the System hmem_info struct
 *
 * @param system_info[out]	System hmem_info struct
 * @return 0 on success
 */
static int efa_hmem_info_init_system(struct efa_hmem_info *system_info, struct efa_domain *efa_domain)
{
	system_info->initialized = true;
	system_info->p2p_disabled_by_user = false;
	system_info->p2p_required_by_impl = false;
	system_info->p2p_supported_by_device = true;
	efa_hmem_info_init_protocol_thresholds(system_info, FI_HMEM_SYSTEM, efa_domain);
	return 0;
}

/**
 * @brief Initialize the Cuda hmem_info struct
 *
 * @param	cuda_info[out]	Cuda hmem_info struct
 * @return	0 on success
 * 		negative libfabric error code on failure
 */
static int efa_hmem_info_init_cuda(struct efa_hmem_info *cuda_info, struct efa_domain *efa_domain)
{
#if HAVE_CUDA
	cudaError_t cuda_ret;
	void *ptr = NULL;
	struct ibv_mr *ibv_mr;
	int ibv_access = IBV_ACCESS_LOCAL_WRITE;
	size_t len = ofi_get_page_size() * 2, tmp_value;
	int ret;

	if (!ofi_hmem_is_initialized(FI_HMEM_CUDA)) {
		EFA_INFO(FI_LOG_DOMAIN, "FI_HMEM_CUDA is not initialized\n");
		return 0;
	}

	if (efa_device_support_rdma_read())
		ibv_access |= IBV_ACCESS_REMOTE_READ;

	cuda_info->initialized = true;

	cuda_ret = ofi_cudaMalloc(&ptr, len);
	if (cuda_ret != cudaSuccess) {
		EFA_WARN(FI_LOG_DOMAIN,
			 "Failed to allocate CUDA buffer: %s\n",
			 ofi_cudaGetErrorString(cuda_ret));
		return -FI_ENOMEM;
	}

	cuda_info->p2p_disabled_by_user = false;

	/*
	 * Require p2p for FI_HMEM_CUDA unless the user exlipictly enables
	 * FI_HMEM_CUDA_ENABLE_XFER
	 */
	cuda_info->p2p_required_by_impl = cuda_get_xfer_setting() != CUDA_XFER_ENABLED;

	ibv_mr = ibv_reg_mr(g_device_list[0].ibv_pd, ptr, len, ibv_access);
	if (!ibv_mr) {
		cuda_info->p2p_supported_by_device = false;
		/* Use FI_HMEM_SYSTEM message sizes when p2p is unavailable */
		efa_hmem_info_init_protocol_thresholds(cuda_info, FI_HMEM_SYSTEM, efa_domain);
		EFA_WARN(FI_LOG_DOMAIN,
			 "Failed to register CUDA buffer with the EFA device, FI_HMEM transfers that require peer to peer support will fail.\n");
		ofi_cudaFree(ptr);
		return 0;
	}

	ret = ibv_dereg_mr(ibv_mr);
	ofi_cudaFree(ptr);
	if (ret) {
		EFA_WARN(FI_LOG_DOMAIN,
			 "Failed to deregister CUDA buffer: %s\n",
			 fi_strerror(-ret));
		return ret;
	}

	cuda_info->p2p_supported_by_device = true;
	efa_hmem_info_init_protocol_thresholds(cuda_info, FI_HMEM_CUDA, efa_domain);
	if (-FI_ENODATA != fi_param_get(&rxr_prov, "inter_max_medium_message_size", &tmp_value)) {
		EFA_WARN(FI_LOG_DOMAIN,
		         "The environment variable FI_EFA_INTER_MAX_MEDIUM_MESSAGE_SIZE was set, "
		         "but EFA HMEM via Cuda API only supports eager and runting read protocols. "
				 "The variable will not modify Cuda memory run config.\n");
	}

#endif
	return 0;
}

/**
 * @brief Initialize the Neuron hmem_info struct
 *
 * @param	neuron_info[out]	Neuron hmem_info struct
 * @return	0 on success
 * 		negative libfabric error code on failure
 */
static int efa_hmem_info_init_neuron(struct efa_hmem_info *neuron_info, struct efa_domain *efa_domain)
{
#if HAVE_NEURON
	struct ibv_mr *ibv_mr;
	int ibv_access = IBV_ACCESS_LOCAL_WRITE;
	void *handle;
	void *ptr = NULL;
	size_t len = ofi_get_page_size() * 2, tmp_value;
	int ret;

	if (!ofi_hmem_is_initialized(FI_HMEM_NEURON)) {
		EFA_INFO(FI_LOG_DOMAIN, "FI_HMEM_NEURON is not initialized\n");
		return 0;
	}

	if (g_device_list[0].device_caps & EFADV_DEVICE_ATTR_CAPS_RDMA_READ) {
		ibv_access |= IBV_ACCESS_REMOTE_READ;
	} else {
		EFA_WARN(FI_LOG_DOMAIN,
			 "No EFA RDMA read support, transfers using AWS Neuron will fail.\n");
		return 0;
	}

	ptr = neuron_alloc(&handle, len);
	/*
	 * neuron_alloc will fail if application did not call nrt_init,
	 * which is ok if it's not running neuron workloads. libfabric
	 * will move on and leave neuron_info->initialized as false.
	 */
	if (!ptr) {
		EFA_INFO(FI_LOG_DOMAIN, "Cannot allocate Neuron buffer\n");
		return 0;
	}

	neuron_info->initialized = true;
	neuron_info->p2p_disabled_by_user = false;
	/* Neuron currently requires P2P */
	neuron_info->p2p_required_by_impl = true;

	ibv_mr = ibv_reg_mr(g_device_list[0].ibv_pd, ptr, len, ibv_access);
	if (!ibv_mr) {
		neuron_info->p2p_supported_by_device = false;
		/* We do not expect to support Neuron on non p2p systems */
		EFA_WARN(FI_LOG_DOMAIN,
		         "Failed to register Neuron buffer with the EFA device, "
		         "FI_HMEM transfers that require peer to peer support will fail.\n");
		neuron_free(&handle);
		return 0;
	}

	ret = ibv_dereg_mr(ibv_mr);
	neuron_free(&handle);
	if (ret) {
		EFA_WARN(FI_LOG_DOMAIN,
			 "Failed to deregister Neuron buffer: %s\n",
			 fi_strerror(-ret));
		return ret;
	}

	neuron_info->p2p_supported_by_device = true;
	efa_hmem_info_init_protocol_thresholds(neuron_info, FI_HMEM_NEURON, efa_domain);
	if (-FI_ENODATA != fi_param_get(&rxr_prov, "inter_max_medium_message_size", &tmp_value)) {
		EFA_WARN(FI_LOG_DOMAIN,
		         "The environment variable FI_EFA_INTER_MAX_MEDIUM_MESSAGE_SIZE was set, "
		         "but EFA HMEM via Neuron API only supports eager and runting read protocols. "
				 "The variable will not modify Neuron memory run config.\n");
	}

#endif
	return 0;
}

/**
 * @brief Initialize the Synapseai hmem_info struct
 *
 * @param synapseai_info[out]	Synapseai hmem_info struct
 * @return 0 on success
 */
static int efa_hmem_info_init_synapseai(struct efa_hmem_info *synapseai_info, struct efa_domain *efa_domain)
{
#if HAVE_SYNAPSEAI
	size_t tmp_value;

	if (!ofi_hmem_is_initialized(FI_HMEM_SYNAPSEAI)) {
		EFA_INFO(FI_LOG_DOMAIN, "FI_HMEM_SYNAPSEAI is not initialized\n");
		return 0;
	}

	if (!(g_device_list[0].device_caps & EFADV_DEVICE_ATTR_CAPS_RDMA_READ)) {
		EFA_WARN(FI_LOG_DOMAIN,
			 "No EFA RDMA read support, transfers using Habana Gaudi will fail.\n");
		return 0;
	}

	synapseai_info->initialized = true;
	synapseai_info->p2p_disabled_by_user = false;
	/* SynapseAI currently requires P2P */
	synapseai_info->p2p_required_by_impl = true;
	synapseai_info->p2p_supported_by_device = true;
	efa_hmem_info_init_protocol_thresholds(synapseai_info, FI_HMEM_SYNAPSEAI, efa_domain);

	/*  Only the long read protocol is supported */
	if (-FI_ENODATA != fi_param_get_size_t(&rxr_prov, "inter_max_medium_message_size", &tmp_value) ||
		-FI_ENODATA != fi_param_get_size_t(&rxr_prov, "inter_min_read_message_size", &tmp_value) ||
		-FI_ENODATA != fi_param_get_size_t(&rxr_prov, "inter_min_read_write_size", &tmp_value) ||
		-FI_ENODATA != fi_param_get_size_t(&rxr_prov, "runt_size", &tmp_value)) {
		EFA_WARN(FI_LOG_DOMAIN,
				"One or more of the following environment variable(s) were set: ["
				"FI_EFA_INTER_MAX_MEDIUM_MESSAGE_SIZE, "
				"FI_EFA_INTER_MIN_READ_MESSAGE_SIZE, "
				"FI_EFA_INTER_MIN_READ_WRITE_SIZE, "
				"FI_EFA_RUNT_SIZE"
				"], but EFA HMEM via Synapse only supports long read protocol. "
				"The variable(s) will not modify Synapse memory run config.\n");
	}

#endif
	return 0;
}

/**
 * @brief   Validate an FI_OPT_FI_HMEM_P2P (FI_OPT_ENDPOINT) option for a
 *          specified HMEM interface.
 *          Also update hmem_info[iface]->p2p_disabled_by_user accordingly.
 *
 * @param   domain  The efa_domain struct which contains an efa_hmem_info array
 * @param   iface   The fi_hmem_iface enum of the HMEM interface to validate
 * @param   p2p_opt The P2P option to validate
 *
 * @return  0 if the P2P option is valid for the given interface
 *         -FI_OPNOTSUPP if the P2P option is invalid
 *         -FI_ENODATA if the given HMEM interface was not initialized
 *         -FI_EINVAL if p2p_opt is not a valid FI_OPT_FI_HMEM_P2P option
 */
int efa_hmem_validate_p2p_opt(struct efa_domain *efa_domain, enum fi_hmem_iface iface, int p2p_opt)
{
	struct efa_hmem_info *info = &efa_domain->hmem_info[iface];

	if (OFI_UNLIKELY(!info->initialized))
		return -FI_ENODATA;

	switch (p2p_opt) {
	case FI_HMEM_P2P_REQUIRED:
		if (!info->p2p_supported_by_device)
			return -FI_EOPNOTSUPP;

		info->p2p_disabled_by_user = false;
		return 0;
	/*
	 * According to fi_setopt() document:
	 *
	 *     ENABLED means a provider may use P2P.
	 *     PREFERED means a provider should prefer P2P if it is available.
	 *
	 * These options does not require that p2p is supported by device,
	 * nor do they prohibit that p2p is reqruied by implementation. Therefore
	 * they are always supported.
	 */
	case FI_HMEM_P2P_PREFERRED:
	case FI_HMEM_P2P_ENABLED:
		info->p2p_disabled_by_user = false;
		return 0;

	case FI_HMEM_P2P_DISABLED:
		if (info->p2p_required_by_impl)
			return -FI_EOPNOTSUPP;

		info->p2p_disabled_by_user = true;
		return 0;
	}

	return -FI_EINVAL;
}

/**
 * @brief Initialize the hmem_info structs for
 * all of the HMEM devices. The device hmem_info
 * struct will be used to determine which efa transfer
 * protocol should be selected.
 *
 * @param   efa_domain[out]     The EFA Domain containing efa_hmem_info
 *                              structures
 * @return  0 on success
 *          negative libfabric error code on an unexpected error
 */
int efa_hmem_info_init_all(struct efa_domain *efa_domain)
{
	int ret, err;
	struct efa_hmem_info *hmem_info = efa_domain->hmem_info;

	if(g_device_cnt <= 0) {
		return -FI_ENODEV;
	}

	memset(hmem_info, 0, OFI_HMEM_MAX * sizeof(struct efa_hmem_info));

	ret = 0;

	err = efa_hmem_info_init_system(&hmem_info[FI_HMEM_SYSTEM], efa_domain);
	if (err) {
		ret = err;
		EFA_WARN(FI_LOG_DOMAIN, "Failed to populate the System hmem_info struct! err: %d\n",
			 err);
	}

	err = efa_hmem_info_init_cuda(&hmem_info[FI_HMEM_CUDA], efa_domain);
	if (err) {
		ret = err;
		EFA_WARN(FI_LOG_DOMAIN, "Failed to populate the Cuda hmem_info struct! err: %d\n",
			 err);
	}

	err = efa_hmem_info_init_neuron(&hmem_info[FI_HMEM_NEURON], efa_domain);
	if (err) {
		ret = err;
		EFA_WARN(FI_LOG_DOMAIN, "Failed to populate the Neuron hmem_info struct! err: %d\n",
			 err);
	}

	err = efa_hmem_info_init_synapseai(&hmem_info[FI_HMEM_SYNAPSEAI], efa_domain);
	if (err) {
		ret = err;
		EFA_WARN(FI_LOG_DOMAIN, "Failed to populate the Synapseai hmem_info struct! err: %d\n",
			 err);
	}

	return ret;
}

/**
 * @brief Copy data from a hmem IOV to a system buffer
 *
 * @param[in]   desc          Array of memory desc corresponding to IOV buffers
 * @param[out]  buff          Target buffer (system memory)
 * @param[in]   buff_size     The size of the target buffer
 * @param[in]   hmem_iov      IOV data source
 * @param[in]   iov_count     Number of IOV structures in IOV array
 * @return  number of bytes copied on success, -FI_ETRUNC on error
 */
ssize_t efa_copy_from_hmem_iov(void **desc, char *buff, int buff_size,
                               const struct iovec *hmem_iov, int iov_count)
{
	uint64_t device;
	enum fi_hmem_iface hmem_iface;
	int i;
	size_t data_size = 0;

	for (i = 0; i < iov_count; i++) {
		if (desc && desc[i]) {
			hmem_iface = ((struct efa_mr **) desc)[i]->peer.iface;
			device = ((struct efa_mr **) desc)[i]->peer.device.reserved;
		} else {
			hmem_iface = FI_HMEM_SYSTEM;
			device = 0;
		}

		if (data_size + hmem_iov[i].iov_len < buff_size) {
			ofi_copy_from_hmem(hmem_iface, device, buff + data_size, hmem_iov[i].iov_base,
		                       hmem_iov[i].iov_len);
			data_size += hmem_iov[i].iov_len;
		} else {
			FI_WARN(&rxr_prov, FI_LOG_CQ, "IOV larger is larger than the target buffer");
			return -FI_ETRUNC;
		}
	}

	return data_size;
}

/**
 * @brief Copy data from a system buffer to a hmem IOV
 *
 * @param[in]    desc            Array of memory desc corresponding to IOV buffers
 * @param[out]   hmem_iov        Target IOV (HMEM)
 * @param[in]    iov_count       Number of IOV entries in vector
 * @param[in]    buff            System buffer data source
 * @param[in]    buff_size       Size of data to copy
 * @return  number of bytes copied on success, -FI_ETRUNC on error
 */
ssize_t efa_copy_to_hmem_iov(void **desc, struct iovec *hmem_iov, int iov_count,
                             char *buff, int buff_size)
{
	uint64_t device;
	enum fi_hmem_iface hmem_iface;
	int i, bytes_remaining = buff_size, size;

	for (i = 0; i < iov_count && bytes_remaining; i++) {
		if (desc && desc[i]) {
			hmem_iface = ((struct efa_mr **) desc)[i]->peer.iface;
			device = ((struct efa_mr **) desc)[i]->peer.device.reserved;
		} else {
			hmem_iface = FI_HMEM_SYSTEM;
			device = 0;
		}

		size = hmem_iov[i].iov_len;
		if (bytes_remaining < size) {
			size = bytes_remaining;
		}

		ofi_copy_to_hmem(hmem_iface, device, hmem_iov[i].iov_base, buff, size);
		bytes_remaining -= size;
	}

	if (bytes_remaining) {
		FI_WARN(&rxr_prov, FI_LOG_CQ, "Source buffer larger than target IOV");
		return -FI_ETRUNC;
	}
	return buff_size;
}
