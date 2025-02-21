/*
 * qemu_command.c: QEMU command generation
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "qemu_command.h"
#include "qemu_hostdev.h"
#include "qemu_capabilities.h"
#include "qemu_dbus.h"
#include "qemu_interface.h"
#include "qemu_alias.h"
#include "qemu_security.h"
#include "qemu_slirp.h"
#include "qemu_block.h"
#include "cpu/cpu.h"
#include "viralloc.h"
#include "virlog.h"
#include "virarch.h"
#include "virerror.h"
#include "virfile.h"
#include "virnetdev.h"
#include "virnetdevbridge.h"
#include "virqemu.h"
#include "virstring.h"
#include "virtime.h"
#include "viruuid.h"
#include "domain_nwfilter.h"
#include "domain_addr.h"
#include "domain_audit.h"
#include "domain_conf.h"
#include "netdev_bandwidth_conf.h"
#include "snapshot_conf.h"
#include "storage_conf.h"
#include "secret_conf.h"
#include "virnetdevtap.h"
#include "virnetdevopenvswitch.h"
#include "device_conf.h"
#include "storage_source_conf.h"
#include "virtpm.h"
#include "virscsi.h"
#include "virnuma.h"
#include "virgic.h"
#include "virmdev.h"
#include "virdomainsnapshotobjlist.h"
#if defined(__linux__)
# include <linux/capability.h>
#endif
#include "logging/log_manager.h"
#include "logging/log_protocol.h"
#include "virutil.h"
#include "virsecureerase.h"

#include <sys/stat.h>
#include <fcntl.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_command");

VIR_ENUM_DECL(qemuDiskCacheV2);

VIR_ENUM_IMPL(qemuDiskCacheV2,
              VIR_DOMAIN_DISK_CACHE_LAST,
              "default",
              "none",
              "writethrough",
              "writeback",
              "directsync",
              "unsafe",
);

VIR_ENUM_IMPL(qemuVideo,
              VIR_DOMAIN_VIDEO_TYPE_LAST,
              "", /* default value, we shouldn't see this */
              "std",
              "cirrus",
              "vmware",
              "", /* don't support xen */
              "", /* don't support vbox */
              "qxl",
              "", /* don't support parallels */
              "", /* no need for virtio */
              "" /* don't support gop */,
              "" /* 'none' doesn't make sense here */,
              "bochs-display",
              "", /* ramfb can't be used with -vga */
);

VIR_ENUM_IMPL(qemuSoundCodec,
              VIR_DOMAIN_SOUND_CODEC_TYPE_LAST,
              "hda-duplex",
              "hda-micro",
              "hda-output",
);

VIR_ENUM_DECL(qemuControllerModelUSB);

VIR_ENUM_IMPL(qemuControllerModelUSB,
              VIR_DOMAIN_CONTROLLER_MODEL_USB_LAST,
              "piix3-usb-uhci",
              "piix4-usb-uhci",
              "usb-ehci",
              "ich9-usb-ehci1",
              "ich9-usb-uhci1",
              "ich9-usb-uhci2",
              "ich9-usb-uhci3",
              "vt82c686b-usb-uhci",
              "pci-ohci",
              "nec-usb-xhci",
              "qusb1",
              "qusb2",
              "qemu-xhci",
              "none",
);

VIR_ENUM_DECL(qemuNumaPolicy);
VIR_ENUM_IMPL(qemuNumaPolicy,
              VIR_DOMAIN_NUMATUNE_MEM_LAST,
              "bind",
              "preferred",
              "interleave",
              "restrictive",
);

VIR_ENUM_DECL(qemuAudioDriver);
VIR_ENUM_IMPL(qemuAudioDriver,
              VIR_DOMAIN_AUDIO_TYPE_LAST,
              "none",
              "alsa",
              "coreaudio",
              "jack",
              "oss",
              "pa",
              "sdl",
              "spice",
              "wav",
);


static int
qemuBuildObjectCommandlineFromJSON(virBuffer *buf,
                                   virJSONValue *props,
                                   virQEMUCaps *qemuCaps)
{
    const char *type = virJSONValueObjectGetString(props, "qom-type");
    const char *alias = virJSONValueObjectGetString(props, "id");

    if (!type || !alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("missing 'type'(%s) or 'alias'(%s) field of QOM 'object'"),
                       NULLSTR(type), NULLSTR(alias));
        return -1;
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_QAPIFIED)) {
        return virJSONValueToBuffer(props, buf, false);
    } else {
        virBufferAsprintf(buf, "%s,", type);

        return virQEMUBuildCommandLineJSON(props, buf, "qom-type",
                                           virQEMUBuildCommandLineJSONArrayBitmap);
    }
}


/**
 * qemuBuildMasterKeyCommandLine:
 * @cmd: the command to modify
 * @qemuCaps qemu capabilities object
 * @domainLibDir: location to find the master key

 * Formats the command line for a master key if available
 *
 * Returns 0 on success, -1 w/ error message on failure
 */
static int
qemuBuildMasterKeyCommandLine(virCommand *cmd,
                              qemuDomainObjPrivate *priv)
{
    g_autofree char *alias = NULL;
    g_autofree char *path = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) props = NULL;

    /* If the -object secret does not exist, then just return. This just
     * means the domain won't be able to use a secret master key and is
     * not a failure.
     */
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_SECRET)) {
        VIR_INFO("secret object is not supported by this QEMU binary");
        return 0;
    }

    if (!(alias = qemuDomainGetMasterKeyAlias()))
        return -1;

    /* Get the path. NB, the mocked test will not have the created
     * file so we cannot check for existence, which is no different
     * than other command line options which do not check for the
     * existence of socket files before using.
     */
    if (!(path = qemuDomainGetMasterKeyFilePath(priv->libDir)))
        return -1;

    if (qemuMonitorCreateObjectProps(&props, "secret", alias,
                                     "s:format", "raw",
                                     "s:file", path,
                                     NULL) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, priv->qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


/**
 * qemuBuildFDSet:
 * @fd: fd to reassign to the child
 * @idx: index in the fd set
 *
 * Format the parameters for the -add-fd command line option
 * for the given file descriptor. The file descriptor must previously
 * have been 'transferred' in a virCommandPassFDIndex() call,
 * and @idx is the value returned by that call.
 */
static char *
qemuBuildFDSet(int fd, size_t idx)
{
    return g_strdup_printf("set=%zu,fd=%d", idx, fd);
}


/**
 * qemuVirCommandGetFDSet:
 * @cmd: the command to modify
 * @fd: fd to reassign to the child
 *
 * Get the parameters for the QEMU -add-fd command line option
 * for the given file descriptor. The file descriptor must previously
 * have been 'transferred' in a virCommandPassFD() call.
 * This function for example returns "set=10,fd=20".
 */
static char *
qemuVirCommandGetFDSet(virCommand *cmd, int fd)
{
    int idx = virCommandPassFDGetFDIndex(cmd, fd);

    if (idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("file descriptor %d has not been transferred"), fd);
        return NULL;
    }

    return g_strdup_printf("set=%d,fd=%d", idx, fd);
}


/**
 * qemuVirCommandGetDevSet:
 * @cmd: the command to modify
 * @fd: fd to reassign to the child
 *
 * Get the parameters for the QEMU path= parameter where a file
 * descriptor is accessed via a file descriptor set, for example
 * /dev/fdset/10. The file descriptor must previously have been
 * 'transferred' in a virCommandPassFD() call.
 */
static char *
qemuVirCommandGetDevSet(virCommand *cmd, int fd)
{
    int idx = virCommandPassFDGetFDIndex(cmd, fd);

    if (idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("file descriptor %d has not been transferred"), fd);
        return NULL;
    }

    return g_strdup_printf("/dev/fdset/%d", idx);
}


static int
qemuBuildDeviceAddressStr(virBuffer *buf,
                          const virDomainDef *domainDef,
                          virDomainDeviceInfo *info)
{
    g_autofree char *devStr = NULL;
    const char *contAlias = NULL;
    bool contIsPHB = false;
    int contTargetIndex = 0;

    switch ((virDomainDeviceAddressType)info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI: {
        size_t i;

        if (!(devStr = virPCIDeviceAddressAsString(&info->addr.pci)))
            return -1;
        for (i = 0; i < domainDef->ncontrollers; i++) {
            virDomainControllerDef *cont = domainDef->controllers[i];

            if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
                cont->idx == info->addr.pci.bus) {
                contAlias = cont->info.alias;
                contIsPHB = virDomainControllerIsPSeriesPHB(cont);
                contTargetIndex = cont->opts.pciopts.targetIndex;

                if (!contAlias) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("Device alias was not set for PCI "
                                     "controller with index %u required "
                                     "for device at address %s"),
                                   info->addr.pci.bus, devStr);
                    return -1;
                }

                if (virDomainDeviceAliasIsUserAlias(contAlias)) {
                    /* When domain has builtin pci-root controller we don't put it
                     * onto cmd line. Therefore we can't set its alias. In that
                     * case, use the default one. */
                    if (!qemuDomainIsPSeries(domainDef) &&
                        cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT) {
                        if (virQEMUCapsHasPCIMultiBus(domainDef))
                            contAlias = "pci.0";
                        else
                            contAlias = "pci";
                    } else if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT) {
                        contAlias = "pcie.0";
                    }
                }
                break;
            }
        }
        if (!contAlias) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not find PCI "
                             "controller with index %u required "
                             "for device at address %s"),
                           info->addr.pci.bus, devStr);
            return -1;
        }

        if (contIsPHB && contTargetIndex > 0) {
            /* The PCI bus created by a spapr-pci-host-bridge device with
             * alias 'x' will be called 'x.0' rather than 'x'; however,
             * this does not apply to the implicit PHB in a pSeries guest,
             * which always has the hardcoded name 'pci.0' */
            virBufferAsprintf(buf, ",bus=%s.0", contAlias);
        } else {
            /* For all other controllers, the bus name matches the alias
             * of the corresponding controller */
            virBufferAsprintf(buf, ",bus=%s", contAlias);
        }

        if (info->addr.pci.multi == VIR_TRISTATE_SWITCH_ON)
            virBufferAddLit(buf, ",multifunction=on");
        else if (info->addr.pci.multi == VIR_TRISTATE_SWITCH_OFF)
            virBufferAddLit(buf, ",multifunction=off");
        virBufferAsprintf(buf, ",addr=0x%x", info->addr.pci.slot);
        if (info->addr.pci.function != 0)
            virBufferAsprintf(buf, ".0x%x", info->addr.pci.function);
        if (info->acpiIndex != 0)
            virBufferAsprintf(buf, ",acpi-index=%u", info->acpiIndex);
    }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
        if (!(contAlias = virDomainControllerAliasFind(domainDef,
                                                       VIR_DOMAIN_CONTROLLER_TYPE_USB,
                                                       info->addr.usb.bus)))
            return -1;
        virBufferAsprintf(buf, ",bus=%s.0", contAlias);
        if (virDomainUSBAddressPortIsValid(info->addr.usb.port)) {
            virBufferAddLit(buf, ",port=");
            virDomainUSBAddressPortFormatBuf(buf, info->addr.usb.port);
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO:
        if (info->addr.spaprvio.has_reg)
            virBufferAsprintf(buf, ",reg=0x%08llx", info->addr.spaprvio.reg);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        if (info->addr.ccw.assigned)
            virBufferAsprintf(buf, ",devno=%x.%x.%04x",
                              info->addr.ccw.cssid,
                              info->addr.ccw.ssid,
                              info->addr.ccw.devno);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA:
        virBufferAsprintf(buf, ",iobase=0x%x,irq=0x%x",
                          info->addr.isa.iobase,
                          info->addr.isa.irq);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM:
        virBufferAsprintf(buf, ",slot=%d", info->addr.dimm.slot);
        if (info->addr.dimm.base)
            virBufferAsprintf(buf, ",addr=%llu", info->addr.dimm.base);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_MMIO:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_UNASSIGNED:
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_LAST:
        virReportEnumRangeError(virDomainDeviceAddressType, info->type);
        return -1;
    }

    return 0;
}


/**
 * qemuBuildVirtioDevStr
 * @buf: virBuffer * to append the built string
 * @baseName: qemu virtio device basename string. Ex: virtio-rng for <rng>
 * @qemuCaps: virQEMUCapPtr
 * @devtype: virDomainDeviceType of the device. Ex: VIR_DOMAIN_DEVICE_TYPE_RNG
 * @devdata: *Def * of the device definition
 *
 * Build the qemu virtio -device name from the passed parameters. Currently
 * this is mostly about attaching the correct string prefix to @baseName for
 * the passed @type. So for @baseName "virtio-rng" and devdata->info.type
 * VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI, generate "virtio-rng-pci"
 *
 * Returns: -1 on failure, 0 on success
 */
static int
qemuBuildVirtioDevStr(virBuffer *buf,
                      const char *baseName,
                      virQEMUCaps *qemuCaps,
                      virDomainDeviceType devtype,
                      void *devdata)
{
    const char *implName = NULL;
    virDomainDeviceDef device = { .type = devtype };
    virDomainDeviceInfo *info;
    bool has_tmodel, has_ntmodel;

    virDomainDeviceSetData(&device, devdata);
    info = virDomainDeviceGetInfo(&device);

    switch ((virDomainDeviceAddressType) info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        implName = "pci";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_MMIO:
        implName = "device";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        implName = "ccw";
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected address type for '%s'"), baseName);
        return -1;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_UNASSIGNED:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainDeviceAddressType, info->type);
        return -1;
    }

    virBufferAsprintf(buf, "%s-%s", baseName, implName);

    switch (devtype) {
        case VIR_DOMAIN_DEVICE_DISK:
            has_tmodel = device.data.disk->model == VIR_DOMAIN_DISK_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.disk->model == VIR_DOMAIN_DISK_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_NET:
            has_tmodel = device.data.net->model == VIR_DOMAIN_NET_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.net->model == VIR_DOMAIN_NET_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_HOSTDEV:
            if (device.data.hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST)
                return 0;
            has_tmodel = device.data.hostdev->source.subsys.u.scsi_host.model == VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_VHOST_MODEL_TYPE_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.hostdev->source.subsys.u.scsi_host.model == VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_VHOST_MODEL_TYPE_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_RNG:
            has_tmodel = device.data.rng->model == VIR_DOMAIN_RNG_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.rng->model == VIR_DOMAIN_RNG_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_FS:
            has_tmodel = device.data.fs->model == VIR_DOMAIN_FS_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.fs->model == VIR_DOMAIN_FS_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_MEMBALLOON:
            has_tmodel = device.data.memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_VSOCK:
            has_tmodel = device.data.vsock->model == VIR_DOMAIN_VSOCK_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.vsock->model == VIR_DOMAIN_VSOCK_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_INPUT:
            if (device.data.input->type != VIR_DOMAIN_INPUT_TYPE_PASSTHROUGH)
                return 0;
            has_tmodel = device.data.input->model == VIR_DOMAIN_INPUT_MODEL_VIRTIO_TRANSITIONAL;
            has_ntmodel = device.data.input->model == VIR_DOMAIN_INPUT_MODEL_VIRTIO_NON_TRANSITIONAL;
            break;

        case VIR_DOMAIN_DEVICE_CONTROLLER:
            if (device.data.controller->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
                has_tmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_VIRTIO_SERIAL_VIRTIO_TRANSITIONAL;
                has_ntmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_VIRTIO_SERIAL_VIRTIO_NON_TRANSITIONAL;
            } else if (device.data.controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
                has_tmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_TRANSITIONAL;
                has_ntmodel = device.data.controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_NON_TRANSITIONAL;
            } else {
                return 0;
            }
            break;

        case VIR_DOMAIN_DEVICE_LEASE:
        case VIR_DOMAIN_DEVICE_SOUND:
        case VIR_DOMAIN_DEVICE_VIDEO:
        case VIR_DOMAIN_DEVICE_WATCHDOG:
        case VIR_DOMAIN_DEVICE_GRAPHICS:
        case VIR_DOMAIN_DEVICE_HUB:
        case VIR_DOMAIN_DEVICE_REDIRDEV:
        case VIR_DOMAIN_DEVICE_NONE:
        case VIR_DOMAIN_DEVICE_SMARTCARD:
        case VIR_DOMAIN_DEVICE_CHR:
        case VIR_DOMAIN_DEVICE_NVRAM:
        case VIR_DOMAIN_DEVICE_SHMEM:
        case VIR_DOMAIN_DEVICE_TPM:
        case VIR_DOMAIN_DEVICE_PANIC:
        case VIR_DOMAIN_DEVICE_MEMORY:
        case VIR_DOMAIN_DEVICE_IOMMU:
        case VIR_DOMAIN_DEVICE_AUDIO:
        case VIR_DOMAIN_DEVICE_LAST:
        default:
            return 0;
    }

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI &&
        (has_tmodel || has_ntmodel)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("virtio (non-)transitional models are not "
                         "supported for address type=%s"),
                       virDomainDeviceAddressTypeToString(info->type));
        return -1;
    }

    if (has_tmodel) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_PCI_TRANSITIONAL)) {
            virBufferAddLit(buf, "-transitional");
        } else if (virQEMUCapsGet(qemuCaps,
                                  QEMU_CAPS_VIRTIO_PCI_DISABLE_LEGACY)) {
            virBufferAddLit(buf, ",disable-legacy=off,disable-modern=off");
        }
        /* No error if -transitional is not supported: our address
         * allocation will force the device into plain PCI bus, which
         * is functionally identical to standard 'virtio-XXX' behavior
         */
    } else if (has_ntmodel) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_PCI_TRANSITIONAL)) {
            virBufferAddLit(buf, "-non-transitional");
        } else if (virQEMUCapsGet(qemuCaps,
                                  QEMU_CAPS_VIRTIO_PCI_DISABLE_LEGACY)) {
            /* Even if the QEMU binary doesn't support the non-transitional
             * device, we can still make it work by manually disabling legacy
             * VirtIO and enabling modern VirtIO */
            virBufferAddLit(buf, ",disable-legacy=on,disable-modern=off");
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio non-transitional model not supported "
                             "for this qemu"));
            return -1;
        }
    }

    return 0;
}

static void
qemuBuildVirtioOptionsStr(virBuffer *buf,
                          virDomainVirtioOptions *virtio)
{
    if (!virtio)
        return;

    if (virtio->iommu != VIR_TRISTATE_SWITCH_ABSENT) {
        virBufferAsprintf(buf, ",iommu_platform=%s",
                          virTristateSwitchTypeToString(virtio->iommu));
    }
    if (virtio->ats != VIR_TRISTATE_SWITCH_ABSENT) {
        virBufferAsprintf(buf, ",ats=%s",
                          virTristateSwitchTypeToString(virtio->ats));
    }
    if (virtio->packed != VIR_TRISTATE_SWITCH_ABSENT) {
        virBufferAsprintf(buf, ",packed=%s",
                          virTristateSwitchTypeToString(virtio->packed));
    }
}

static int
qemuBuildRomStr(virBuffer *buf,
                virDomainDeviceInfo *info)
{
    if (info->romenabled || info->rombar || info->romfile) {
        if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("ROM tuning is only supported for PCI devices"));
            return -1;
        }

        /* Passing an empty romfile= tells QEMU to disable ROM entirely for
         * this device, and makes other settings irrelevant */
        if (info->romenabled == VIR_TRISTATE_BOOL_NO) {
            virBufferAddLit(buf, ",romfile=");
            return 0;
        }

        switch (info->rombar) {
        case VIR_TRISTATE_SWITCH_OFF:
            virBufferAddLit(buf, ",rombar=0");
            break;
        case VIR_TRISTATE_SWITCH_ON:
            virBufferAddLit(buf, ",rombar=1");
            break;
        case VIR_TRISTATE_SWITCH_ABSENT:
        case VIR_TRISTATE_SWITCH_LAST:
            break;
        }
        if (info->romfile) {
           virBufferAddLit(buf, ",romfile=");
           virQEMUBuildBufferEscapeComma(buf, info->romfile);
        }
    }

    return 0;
}

static int
qemuBuildIoEventFdStr(virBuffer *buf,
                      virTristateSwitch use,
                      virQEMUCaps *qemuCaps)
{
    if (use && virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_IOEVENTFD))
        virBufferAsprintf(buf, ",ioeventfd=%s",
                          virTristateSwitchTypeToString(use));
    return 0;
}

/**
 * qemuBuildSecretInfoProps:
 * @secinfo: pointer to the secret info object
 * @props: json properties to return
 *
 * Build the JSON properties for the secret info type.
 *
 * Returns 0 on success with the filled in JSON property; otherwise,
 * returns -1 on failure error message set.
 */
int
qemuBuildSecretInfoProps(qemuDomainSecretInfo *secinfo,
                         virJSONValue **propsret)
{
    g_autofree char *keyid = NULL;

    if (!(keyid = qemuDomainGetMasterKeyAlias()))
        return -1;

    return qemuMonitorCreateObjectProps(propsret, "secret",
                                        secinfo->s.aes.alias, "s:data",
                                        secinfo->s.aes.ciphertext, "s:keyid",
                                        keyid, "s:iv", secinfo->s.aes.iv,
                                        "s:format", "base64", NULL);
}


/**
 * qemuBuildObjectSecretCommandLine:
 * @cmd: the command to modify
 * @secinfo: pointer to the secret info object
 * @qemuCaps: qemu capabilities
 *
 * If the secinfo is available and associated with an AES secret,
 * then format the command line for the secret object. This object
 * will be referenced by the device that needs/uses it, so it needs
 * to be in place first.
 *
 * Returns 0 on success, -1 w/ error message on failure
 */
static int
qemuBuildObjectSecretCommandLine(virCommand *cmd,
                                 qemuDomainSecretInfo *secinfo,
                                 virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) props = NULL;

    if (qemuBuildSecretInfoProps(secinfo, &props) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


/* qemuBuildGeneralSecinfoURI:
 * @uri: Pointer to the URI structure to add to
 * @secinfo: Pointer to the secret info data (if present)
 *
 * If we have a secinfo, then build the command line options for
 * the secret info for the "general" case (somewhat a misnomer since
 * an iscsi disk is the only one with a secinfo).
 *
 * Returns 0 on success or if no secinfo,
 * -1 and error message if fail to add secret information
 */
static int
qemuBuildGeneralSecinfoURI(virURI *uri,
                           qemuDomainSecretInfo *secinfo)
{
    if (!secinfo)
        return 0;

    switch ((qemuDomainSecretInfoType) secinfo->type) {
    case VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN:
        if (secinfo->s.plain.secret) {
            if (!virStringBufferIsPrintable(secinfo->s.plain.secret,
                                            secinfo->s.plain.secretlen)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("found non printable characters in secret"));
                return -1;
            }
            uri->user = g_strdup_printf("%s:%s", secinfo->s.plain.username,
                                        secinfo->s.plain.secret);
        } else {
            uri->user = g_strdup(secinfo->s.plain.username);
        }
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_AES:
    case VIR_DOMAIN_SECRET_INFO_TYPE_LAST:
        return -1;
    }

    return 0;
}


/* qemuBuildRBDSecinfoURI:
 * @uri: Pointer to the URI structure to add to
 * @secinfo: Pointer to the secret info data (if present)
 *
 * If we have a secinfo, then build the command line options for
 * the secret info for the RBD network storage. Assumption for this
 * is both username and secret exist for plaintext
 *
 * Returns 0 on success or if no secinfo,
 * -1 and error message if fail to add secret information
 */
static int
qemuBuildRBDSecinfoURI(virBuffer *buf,
                       qemuDomainSecretInfo *secinfo)
{
    g_autofree char *base64secret = NULL;

    if (!secinfo) {
        virBufferAddLit(buf, ":auth_supported=none");
        return 0;
    }

    switch ((qemuDomainSecretInfoType) secinfo->type) {
    case VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN:
        base64secret = g_base64_encode(secinfo->s.plain.secret,
                                       secinfo->s.plain.secretlen);
        virBufferEscape(buf, '\\', ":", ":id=%s", secinfo->s.plain.username);
        virBufferEscape(buf, '\\', ":",
                        ":key=%s:auth_supported=cephx\\;none",
                        base64secret);
        virSecureEraseString(base64secret);
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_AES:
        virBufferEscape(buf, '\\', ":", ":id=%s:auth_supported=cephx\\;none",
                        secinfo->s.aes.username);
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_LAST:
        return -1;
    }

    return 0;
}


/* qemuBuildTLSx509BackendProps:
 * @tlspath: path to the TLS credentials
 * @listen: boolean listen for client or server setting
 * @verifypeer: boolean to enable peer verification (form of authorization)
 * @alias: alias for the TLS credentials object
 * @secalias: if one exists, the alias of the security object for passwordid
 * @qemuCaps: capabilities
 * @propsret: json properties to return
 *
 * Create a backend string for the tls-creds-x509 object.
 *
 * Returns 0 on success, -1 on failure with error set.
 */
int
qemuBuildTLSx509BackendProps(const char *tlspath,
                             bool isListen,
                             bool verifypeer,
                             const char *alias,
                             const char *secalias,
                             virQEMUCaps *qemuCaps,
                             virJSONValue **propsret)
{
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_TLS_CREDS_X509)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("tls-creds-x509 not supported in this QEMU binary"));
        return -1;
    }

    if (qemuMonitorCreateObjectProps(propsret, "tls-creds-x509", alias,
                                     "s:dir", tlspath,
                                     "s:endpoint", (isListen ? "server": "client"),
                                     "b:verify-peer", (isListen ? verifypeer : true),
                                     "S:passwordid", secalias,
                                     NULL) < 0)
        return -1;

    return 0;
}


/* qemuBuildTLSx509CommandLine:
 * @cmd: Pointer to command
 * @tlspath: path to the TLS credentials
 * @listen: boolean listen for client or server setting
 * @verifypeer: boolean to enable peer verification (form of authorization)
 * @certEncSecretAlias: alias of a 'secret' object for decrypting TLS private key
 *                      (optional)
 * @alias: TLS object alias
 * @qemuCaps: capabilities
 *
 * Create the command line for a TLS object
 *
 * Returns 0 on success, -1 on failure with error set.
 */
static int
qemuBuildTLSx509CommandLine(virCommand *cmd,
                            const char *tlspath,
                            bool isListen,
                            bool verifypeer,
                            const char *certEncSecretAlias,
                            const char *alias,
                            virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) props = NULL;

    if (qemuBuildTLSx509BackendProps(tlspath, isListen, verifypeer, alias,
                                     certEncSecretAlias, qemuCaps, &props) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


static char *
qemuBuildNetworkDriveURI(virStorageSource *src,
                         qemuDomainSecretInfo *secinfo)
{
    g_autoptr(virURI) uri = NULL;

    if (!(uri = qemuBlockStorageSourceGetURI(src)))
        return NULL;

    if (src->hosts->socket)
        uri->query = g_strdup_printf("socket=%s", src->hosts->socket);

    if (qemuBuildGeneralSecinfoURI(uri, secinfo) < 0)
        return NULL;

    return virURIFormat(uri);
}


static char *
qemuBuildNetworkDriveStr(virStorageSource *src,
                         qemuDomainSecretInfo *secinfo)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;
    char *ret = NULL;

    switch ((virStorageNetProtocol) src->protocol) {
        case VIR_STORAGE_NET_PROTOCOL_NBD:
            if (src->nhosts != 1) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("protocol '%s' accepts only one host"),
                               virStorageNetProtocolTypeToString(src->protocol));
                return NULL;
            }

            if (!((src->hosts->name && strchr(src->hosts->name, ':')) ||
                  (src->hosts->transport == VIR_STORAGE_NET_HOST_TRANS_TCP &&
                   !src->hosts->name) ||
                  (src->hosts->transport == VIR_STORAGE_NET_HOST_TRANS_UNIX &&
                   src->hosts->socket &&
                   !g_path_is_absolute(src->hosts->socket)))) {

                virBufferAddLit(&buf, "nbd:");

                switch (src->hosts->transport) {
                case VIR_STORAGE_NET_HOST_TRANS_TCP:
                    virBufferAsprintf(&buf, "%s:%u",
                                      src->hosts->name, src->hosts->port);
                    break;

                case VIR_STORAGE_NET_HOST_TRANS_UNIX:
                    if (!src->hosts->socket) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("socket attribute required for "
                                         "unix transport"));
                        return NULL;
                    }

                    virBufferAsprintf(&buf, "unix:%s", src->hosts->socket);
                    break;

                default:
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("nbd does not support transport '%s'"),
                                   virStorageNetHostTransportTypeToString(src->hosts->transport));
                    return NULL;
                }

                if (src->path)
                    virBufferAsprintf(&buf, ":exportname=%s", src->path);

                return virBufferContentAndReset(&buf);
            }
            /* NBD code uses URI formatting scheme as others in some cases */
            ret = qemuBuildNetworkDriveURI(src, secinfo);
            break;

        case VIR_STORAGE_NET_PROTOCOL_HTTP:
        case VIR_STORAGE_NET_PROTOCOL_HTTPS:
        case VIR_STORAGE_NET_PROTOCOL_FTP:
        case VIR_STORAGE_NET_PROTOCOL_FTPS:
        case VIR_STORAGE_NET_PROTOCOL_TFTP:
        case VIR_STORAGE_NET_PROTOCOL_ISCSI:
        case VIR_STORAGE_NET_PROTOCOL_GLUSTER:
            ret = qemuBuildNetworkDriveURI(src, secinfo);
            break;

        case VIR_STORAGE_NET_PROTOCOL_SHEEPDOG:
            if (!src->path) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing disk source for 'sheepdog' protocol"));
                return NULL;
            }

            if (src->nhosts == 0) {
                ret = g_strdup_printf("sheepdog:%s", src->path);
            } else if (src->nhosts == 1) {
                ret = g_strdup_printf("sheepdog:%s:%u:%s", src->hosts->name,
                                      src->hosts->port, src->path);
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("protocol 'sheepdog' accepts up to one host"));
                return NULL;
            }

            break;

        case VIR_STORAGE_NET_PROTOCOL_RBD:
            if (strchr(src->path, ':')) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("':' not allowed in RBD source volume name '%s'"),
                               src->path);
                return NULL;
            }

            virBufferStrcat(&buf, "rbd:", src->volume, "/", src->path, NULL);

            if (src->snapshot)
                virBufferEscape(&buf, '\\', ":", "@%s", src->snapshot);

            if (qemuBuildRBDSecinfoURI(&buf, secinfo) < 0)
                return NULL;

            if (src->nhosts > 0) {
                virBufferAddLit(&buf, ":mon_host=");
                for (i = 0; i < src->nhosts; i++) {
                    if (i)
                        virBufferAddLit(&buf, "\\;");

                    /* assume host containing : is ipv6 */
                    if (strchr(src->hosts[i].name, ':'))
                        virBufferEscape(&buf, '\\', ":", "[%s]",
                                        src->hosts[i].name);
                    else
                        virBufferAsprintf(&buf, "%s", src->hosts[i].name);

                    if (src->hosts[i].port)
                        virBufferAsprintf(&buf, "\\:%u", src->hosts[i].port);
                }
            }

            if (src->configFile)
                virBufferEscape(&buf, '\\', ":", ":conf=%s", src->configFile);

            ret = virBufferContentAndReset(&buf);
            break;

        case VIR_STORAGE_NET_PROTOCOL_VXHS:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("VxHS protocol does not support URI syntax"));
            return NULL;

        case VIR_STORAGE_NET_PROTOCOL_SSH:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("'ssh' protocol is not yet supported"));
            return NULL;

        case VIR_STORAGE_NET_PROTOCOL_NFS:
        case VIR_STORAGE_NET_PROTOCOL_LAST:
        case VIR_STORAGE_NET_PROTOCOL_NONE:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected network protocol '%s'"),
                           virStorageNetProtocolTypeToString(src->protocol));
            return NULL;
    }

    return ret;
}


int
qemuGetDriveSourceString(virStorageSource *src,
                         qemuDomainSecretInfo *secinfo,
                         char **source)
{
    int actualType = virStorageSourceGetActualType(src);

    *source = NULL;

    /* return 1 for empty sources */
    if (virStorageSourceIsEmpty(src))
        return 1;

    switch ((virStorageType)actualType) {
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_FILE:
    case VIR_STORAGE_TYPE_DIR:
        *source = g_strdup(src->path);

        break;

    case VIR_STORAGE_TYPE_NETWORK:
        if (!(*source = qemuBuildNetworkDriveStr(src, secinfo)))
            return -1;
        break;

    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_VHOST_USER:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        break;
    }

    return 0;
}


bool
qemuDiskConfigBlkdeviotuneEnabled(virDomainDiskDef *disk)
{
    return !!disk->blkdeviotune.group_name ||
           virDomainBlockIoTuneInfoHasAny(&disk->blkdeviotune);
}


/* QEMU 1.2 and later have a binary flag -enable-fips that must be
 * used for VNC auth to obey FIPS settings; but the flag only
 * exists on Linux, and with no way to probe for it via QMP.  Our
 * solution: if FIPS mode is required, then unconditionally use
 * the flag, regardless of qemu version, for the following matrix:
 *
 *                          old QEMU            new QEMU
 * FIPS enabled             doesn't start       VNC auth disabled
 * FIPS disabled/missing    VNC auth enabled    VNC auth enabled
 *
 * In QEMU 5.2.0, use of -enable-fips was deprecated. In scenarios
 * where FIPS is required, QEMU must be built against libgcrypt
 * which automatically enforces FIPS compliance.
 */
bool
qemuCheckFips(virDomainObj *vm)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    virQEMUCaps *qemuCaps = priv->qemuCaps;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_ENABLE_FIPS))
        return false;

    if (virFileExists("/proc/sys/crypto/fips_enabled")) {
        g_autofree char *buf = NULL;

        if (virFileReadAll("/proc/sys/crypto/fips_enabled", 10, &buf) < 0)
            return false;
        if (STREQ(buf, "1\n"))
            return true;
    }

    return false;
}


/**
 * qemuDiskBusIsSD:
 * @bus: disk bus
 *
 * Unfortunately it is not possible to use -device for SD devices.
 * Fortunately, those don't need static PCI addresses, so we can use -drive
 * without -device.
 */
bool
qemuDiskBusIsSD(int bus)
{
    return bus == VIR_DOMAIN_DISK_BUS_SD;
}


/**
 * qemuDiskSourceNeedsProps:
 * @src: disk source
 *
 * Returns true, if the disk source needs to be generated from the JSON
 * representation. Otherwise, the disk source should be represented using
 * the legacy representation.
 */
static bool
qemuDiskSourceNeedsProps(virStorageSource *src,
                         virQEMUCaps *qemuCaps)
{
    int actualType = virStorageSourceGetActualType(src);

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_GLUSTER &&
        src->nhosts > 1)
        return true;

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_VXHS)
        return true;

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_ISCSI &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_ISCSI_PASSWORD_SECRET))
        return true;

    if (actualType == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_NBD &&
        src->haveTLS == VIR_TRISTATE_BOOL_YES)
        return true;

    if (actualType == VIR_STORAGE_TYPE_NVME)
        return true;

    return false;
}


/**
 * qemuDiskSourceGetProps:
 * @src: disk source struct
 *
 * Returns the disk source struct wrapped so that it can be used as disk source
 * directly by converting it from json.
 */
static virJSONValue *
qemuDiskSourceGetProps(virStorageSource *src)
{
    g_autoptr(virJSONValue) props = NULL;
    virJSONValue *ret;

    if (!(props = qemuBlockStorageSourceGetBackendProps(src,
                                                        QEMU_BLOCK_STORAGE_SOURCE_BACKEND_PROPS_LEGACY)))
        return NULL;

    if (virJSONValueObjectCreate(&ret, "a:file", &props, NULL) < 0)
        return NULL;

    return ret;
}


static int
qemuBuildDriveSourcePR(virBuffer *buf,
                       virDomainDiskDef *disk)
{
    g_autofree char *alias = NULL;
    const char *defaultAlias = NULL;

    if (!disk->src->pr)
        return 0;

    if (virStoragePRDefIsManaged(disk->src->pr))
        defaultAlias = qemuDomainGetManagedPRAlias();
    else if (!(alias = qemuDomainGetUnmanagedPRAlias(disk->info.alias)))
        return -1;


    virBufferAsprintf(buf, ",file.pr-manager=%s", alias ? alias : defaultAlias);
    return 0;
}


static int
qemuBuildDriveSourceStr(virDomainDiskDef *disk,
                        virQEMUCaps *qemuCaps,
                        virBuffer *buf)
{
    int actualType = virStorageSourceGetActualType(disk->src);
    qemuDomainStorageSourcePrivate *srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(disk->src);
    qemuDomainSecretInfo *secinfo = NULL;
    qemuDomainSecretInfo *encinfo = NULL;
    g_autoptr(virJSONValue) srcprops = NULL;
    g_autofree char *source = NULL;
    bool rawluks = false;

    if (srcpriv) {
        secinfo = srcpriv->secinfo;
        encinfo = srcpriv->encinfo;
    }

    if (qemuDiskSourceNeedsProps(disk->src, qemuCaps) &&
        !(srcprops = qemuDiskSourceGetProps(disk->src)))
        return -1;

    if (!srcprops &&
        qemuGetDriveSourceString(disk->src, secinfo, &source) < 0)
        return -1;

    /* nothing to format if the drive is empty */
    if (!(source || srcprops) ||
        ((disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY ||
          disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM) &&
         disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN)) {
        return 0;
    }

    if (actualType == VIR_STORAGE_TYPE_BLOCK &&
        disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       disk->src->type == VIR_STORAGE_TYPE_VOLUME ?
                       _("tray status 'open' is invalid for block type volume") :
                       _("tray status 'open' is invalid for block type disk"));
        return -1;
    }

    if (source) {
        virBufferAddLit(buf, "file=");

        /* for now the DIR based storage is handled by the magic FAT format */
        if (actualType == VIR_STORAGE_TYPE_DIR) {
            virBufferAddLit(buf, "fat:");

            if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
                virBufferAddLit(buf, "floppy:");
        }

        virQEMUBuildBufferEscapeComma(buf, source);

        if (secinfo && secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES)
            virBufferAsprintf(buf, ",file.password-secret=%s", secinfo->s.aes.alias);

        if (disk->src->debug)
            virBufferAsprintf(buf, ",file.debug=%d", disk->src->debugLevel);

        if (qemuBuildDriveSourcePR(buf, disk) < 0)
            return -1;
    } else {
        if (!(source = virQEMUBuildDriveCommandlineFromJSON(srcprops)))
            return -1;

        virBufferAdd(buf, source, -1);
    }
    virBufferAddLit(buf, ",");

    if (encinfo) {
        if (disk->src->format == VIR_STORAGE_FILE_RAW) {
            virBufferAsprintf(buf, "key-secret=%s,", encinfo->s.aes.alias);
            rawluks = true;
        } else if (disk->src->format == VIR_STORAGE_FILE_QCOW2 &&
                   disk->src->encryption->format == VIR_STORAGE_ENCRYPTION_FORMAT_LUKS) {
            virBufferAddLit(buf, "encrypt.format=luks,");
            virBufferAsprintf(buf, "encrypt.key-secret=%s,", encinfo->s.aes.alias);
        }
    }

    if (disk->src->format > 0 &&
        actualType != VIR_STORAGE_TYPE_DIR) {
        const char *qemuformat = virStorageFileFormatTypeToString(disk->src->format);
        if (rawluks)
            qemuformat = "luks";
        virBufferAsprintf(buf, "format=%s,", qemuformat);
    }

    return 0;
}


static void
qemuBuildDiskThrottling(virDomainDiskDef *disk,
                        virBuffer *buf)
{
#define IOTUNE_ADD(_field, _label) \
    if (disk->blkdeviotune._field) { \
        virBufferAsprintf(buf, ",throttling." _label "=%llu", \
                          disk->blkdeviotune._field); \
    }

    IOTUNE_ADD(total_bytes_sec, "bps-total");
    IOTUNE_ADD(read_bytes_sec, "bps-read");
    IOTUNE_ADD(write_bytes_sec, "bps-write");
    IOTUNE_ADD(total_iops_sec, "iops-total");
    IOTUNE_ADD(read_iops_sec, "iops-read");
    IOTUNE_ADD(write_iops_sec, "iops-write");

    IOTUNE_ADD(total_bytes_sec_max, "bps-total-max");
    IOTUNE_ADD(read_bytes_sec_max, "bps-read-max");
    IOTUNE_ADD(write_bytes_sec_max, "bps-write-max");
    IOTUNE_ADD(total_iops_sec_max, "iops-total-max");
    IOTUNE_ADD(read_iops_sec_max, "iops-read-max");
    IOTUNE_ADD(write_iops_sec_max, "iops-write-max");

    IOTUNE_ADD(size_iops_sec, "iops-size");
    if (disk->blkdeviotune.group_name) {
        virBufferAddLit(buf, ",throttling.group=");
        virQEMUBuildBufferEscapeComma(buf, disk->blkdeviotune.group_name);
    }

    IOTUNE_ADD(total_bytes_sec_max_length, "bps-total-max-length");
    IOTUNE_ADD(read_bytes_sec_max_length, "bps-read-max-length");
    IOTUNE_ADD(write_bytes_sec_max_length, "bps-write-max-length");
    IOTUNE_ADD(total_iops_sec_max_length, "iops-total-max-length");
    IOTUNE_ADD(read_iops_sec_max_length, "iops-read-max-length");
    IOTUNE_ADD(write_iops_sec_max_length, "iops-write-max-length");
#undef IOTUNE_ADD
}


static void
qemuBuildDiskFrontendAttributeErrorPolicy(virDomainDiskDef *disk,
                                          virBuffer *buf)
{
    const char *wpolicy = NULL;
    const char *rpolicy = NULL;

    if (disk->error_policy)
        wpolicy = virDomainDiskErrorPolicyTypeToString(disk->error_policy);

    if (disk->rerror_policy)
        rpolicy = virDomainDiskErrorPolicyTypeToString(disk->rerror_policy);

    if (disk->error_policy == VIR_DOMAIN_DISK_ERROR_POLICY_ENOSPACE) {
        /* in the case of enospace, the option is spelled
         * differently in qemu, and it's only valid for werror,
         * not for rerror, so leave rerror NULL.
         */
        wpolicy = "enospc";
    } else if (!rpolicy) {
        /* for other policies, rpolicy can match wpolicy */
        rpolicy = wpolicy;
    }

    if (wpolicy)
        virBufferAsprintf(buf, ",werror=%s", wpolicy);
    if (rpolicy)
        virBufferAsprintf(buf, ",rerror=%s", rpolicy);
}


static void
qemuBuildDiskFrontendAttributes(virDomainDiskDef *disk,
                                virBuffer *buf)
{
    /* generate geometry command string */
    if (disk->geometry.cylinders > 0 &&
        disk->geometry.heads > 0 &&
        disk->geometry.sectors > 0) {
        virBufferAsprintf(buf, ",cyls=%u,heads=%u,secs=%u",
                          disk->geometry.cylinders,
                          disk->geometry.heads,
                          disk->geometry.sectors);

        if (disk->geometry.trans != VIR_DOMAIN_DISK_TRANS_DEFAULT)
            virBufferAsprintf(buf, ",bios-chs-trans=%s",
                              virDomainDiskGeometryTransTypeToString(disk->geometry.trans));
    }

    if (disk->serial) {
        virBufferAddLit(buf, ",serial=");
        virBufferEscape(buf, '\\', " ", "%s", disk->serial);
    }
}


static char *
qemuBuildDriveStr(virDomainDiskDef *disk,
                  virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;
    int detect_zeroes = virDomainDiskGetDetectZeroesMode(disk->discard,
                                                         disk->detect_zeroes);

    if (qemuBuildDriveSourceStr(disk, qemuCaps, &opt) < 0)
        return NULL;

    if (!qemuDiskBusIsSD(disk->bus)) {
        g_autofree char *drivealias = qemuAliasDiskDriveFromDisk(disk);
        if (!drivealias)
            return NULL;

        virBufferAddLit(&opt, "if=none");
        virBufferAsprintf(&opt, ",id=%s", drivealias);
    } else {
        virBufferAsprintf(&opt, "if=sd,index=%d",
                          virDiskNameToIndex(disk->dst));
    }

    /* werror/rerror are really frontend attributes, but older
     * qemu requires them on -drive instead of -device */
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_STORAGE_WERROR))
        qemuBuildDiskFrontendAttributeErrorPolicy(disk, &opt);

    if (disk->src->readonly)
        virBufferAddLit(&opt, ",readonly=on");

    /* qemu rejects some parameters for an empty -drive, so we need to skip
     * them in that case:
     * cache: modifies properties of the format driver which is not present
     * copy_on_read: really only works for floppies
     * discard: modifies properties of format driver
     * detect_zeroes: works but really depends on discard so it's useless
     * iomode: setting it to 'native' requires a specific cache mode
     */
    if (!virStorageSourceIsEmpty(disk->src)) {
        if (disk->cachemode) {
            virBufferAsprintf(&opt, ",cache=%s",
                              qemuDiskCacheV2TypeToString(disk->cachemode));
        }

        if (disk->copy_on_read) {
            virBufferAsprintf(&opt, ",copy-on-read=%s",
                              virTristateSwitchTypeToString(disk->copy_on_read));
        }

        if (disk->discard) {
            virBufferAsprintf(&opt, ",discard=%s",
                              virDomainDiskDiscardTypeToString(disk->discard));
        }

        if (detect_zeroes) {
            virBufferAsprintf(&opt, ",detect-zeroes=%s",
                              virDomainDiskDetectZeroesTypeToString(detect_zeroes));
        }

        if (disk->iomode) {
            virBufferAsprintf(&opt, ",aio=%s",
                              virDomainDiskIoTypeToString(disk->iomode));
        }
    }

    qemuBuildDiskThrottling(disk, &opt);

    return virBufferContentAndReset(&opt);
}


static int
qemuBuildDriveDevCacheStr(virDomainDiskDef *disk,
                          virBuffer *buf,
                          virQEMUCaps *qemuCaps)
{
    bool wb;

    if (disk->cachemode == VIR_DOMAIN_DISK_CACHE_DEFAULT)
        return 0;

    /* VIR_DOMAIN_DISK_DEVICE_LUN translates into 'scsi-block'
     * where any caching setting makes no sense. */
    if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN)
        return 0;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DISK_WRITE_CACHE))
        return 0;

    if (qemuDomainDiskCachemodeFlags(disk->cachemode, &wb, NULL, NULL) < 0)
        return -1;

    virBufferStrcat(buf, ",write-cache=",
                    virTristateSwitchTypeToString(virTristateSwitchFromBool(wb)),
                    NULL);

    return 0;
}


char *
qemuBuildDiskDeviceStr(const virDomainDef *def,
                       virDomainDiskDef *disk,
                       virQEMUCaps *qemuCaps)
{
    qemuDomainDiskPrivate *diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;
    const char *contAlias;
    g_autofree char *backendAlias = NULL;
    g_autofree char *scsiVPDDeviceId = NULL;
    int controllerModel;

    switch ((virDomainDiskBus) disk->bus) {
    case VIR_DOMAIN_DISK_BUS_IDE:
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
            virBufferAddLit(&opt, "ide-cd");
        else
            virBufferAddLit(&opt, "ide-hd");

        /* When domain has builtin IDE controller we don't put it onto cmd
         * line. Therefore we can't set its alias. In that case, use the
         * default one. */
        if (qemuDomainHasBuiltinIDE(def)) {
            contAlias = "ide";
        } else {
            if (!(contAlias = virDomainControllerAliasFind(def,
                                                           VIR_DOMAIN_CONTROLLER_TYPE_IDE,
                                                           disk->info.addr.drive.controller)))
                return NULL;
        }
        virBufferAsprintf(&opt, ",bus=%s.%d,unit=%d",
                          contAlias,
                          disk->info.addr.drive.bus,
                          disk->info.addr.drive.unit);
        break;

    case VIR_DOMAIN_DISK_BUS_SCSI:
        controllerModel = qemuDomainFindSCSIControllerModel(def, &disk->info);
        if (controllerModel < 0)
            return NULL;

        if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
            virBufferAddLit(&opt, "scsi-block");
        } else {
            if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
                virBufferAddLit(&opt, "scsi-cd");
            else
                virBufferAddLit(&opt, "scsi-hd");

            /* qemu historically used the name of -drive as one of the device
             * ids in the Vital Product Data Device Identification page if
             * disk serial was not set and the disk serial otherwise.
             * To avoid a guest-visible regression we need to provide it
             * ourselves especially for cases when -blockdev will be used */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_DEVICE_ID)) {
                if (disk->serial) {
                    scsiVPDDeviceId = g_strdup(disk->serial);
                } else {
                    if (!(scsiVPDDeviceId = qemuAliasDiskDriveFromDisk(disk)))
                        return NULL;
                }
            }
        }

        if (!(contAlias = virDomainControllerAliasFind(def, VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
                                                       disk->info.addr.drive.controller)))
           return NULL;

        switch ((virDomainControllerModelSCSI)controllerModel) {
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_NCR53C90:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DC390:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AM53C974:
            virBufferAsprintf(&opt, ",bus=%s.%d,scsi-id=%d",
                              contAlias,
                              disk->info.addr.drive.bus,
                              disk->info.addr.drive.unit);
            break;

        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AUTO:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_BUSLOGIC:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1068:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VMPVSCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_TRANSITIONAL:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_NON_TRANSITIONAL:
            virBufferAsprintf(&opt, ",bus=%s.0,channel=%d,scsi-id=%d,lun=%d",
                              contAlias,
                              disk->info.addr.drive.bus,
                              disk->info.addr.drive.target,
                              disk->info.addr.drive.unit);
            break;

        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DEFAULT:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected SCSI controller model %d"),
                           controllerModel);
            return NULL;
        }

        if (scsiVPDDeviceId)
            virBufferStrcat(&opt, ",device_id=", scsiVPDDeviceId, NULL);

        break;

    case VIR_DOMAIN_DISK_BUS_SATA:
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
            virBufferAddLit(&opt, "ide-cd");
        else
            virBufferAddLit(&opt, "ide-hd");

        /* When domain has builtin SATA controller we don't put it onto cmd
         * line. Therefore we can't set its alias. In that case, use the
         * default one. */
        if (qemuDomainIsQ35(def) &&
            disk->info.addr.drive.controller == 0) {
            contAlias = "ide";
        } else {
            if (!(contAlias = virDomainControllerAliasFind(def,
                                                           VIR_DOMAIN_CONTROLLER_TYPE_SATA,
                                                           disk->info.addr.drive.controller)))
                return NULL;
        }
        virBufferAsprintf(&opt, ",bus=%s.%d",
                          contAlias,
                          disk->info.addr.drive.unit);
        break;

    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        if (virStorageSourceGetActualType(disk->src) == VIR_STORAGE_TYPE_VHOST_USER) {
            if (qemuBuildVirtioDevStr(&opt, "vhost-user-blk", qemuCaps,
                                      VIR_DOMAIN_DEVICE_DISK, disk) < 0) {
                return NULL;
            }
        } else {
            if (qemuBuildVirtioDevStr(&opt, "virtio-blk", qemuCaps,
                                      VIR_DOMAIN_DEVICE_DISK, disk) < 0) {
                return NULL;
            }
        }

        if (disk->iothread)
            virBufferAsprintf(&opt, ",iothread=iothread%u", disk->iothread);

        qemuBuildIoEventFdStr(&opt, disk->ioeventfd, qemuCaps);
        if (disk->event_idx &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_EVENT_IDX)) {
            virBufferAsprintf(&opt, ",event_idx=%s",
                              virTristateSwitchTypeToString(disk->event_idx));
        }
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SCSI) &&
            !(virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SCSI_DEFAULT_DISABLED) &&
              disk->device != VIR_DOMAIN_DISK_DEVICE_LUN)) {
            /* if sg_io is true but the scsi option isn't supported,
             * that means it's just always on in this version of qemu.
             */
            virBufferAsprintf(&opt, ",scsi=%s",
                              (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN)
                              ? "on" : "off");
        }

        if (disk->queues) {
            virBufferAsprintf(&opt, ",num-queues=%u", disk->queues);
        }

        qemuBuildVirtioOptionsStr(&opt, disk->virtio);

        if (qemuBuildDeviceAddressStr(&opt, def, &disk->info) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_DISK_BUS_USB:
        virBufferAddLit(&opt, "usb-storage");

        if (qemuBuildDeviceAddressStr(&opt, def, &disk->info) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_DISK_BUS_FDC:
        virBufferAsprintf(&opt, "floppy,unit=%d", disk->info.addr.drive.unit);
        break;

    case VIR_DOMAIN_DISK_BUS_XEN:
    case VIR_DOMAIN_DISK_BUS_UML:
    case VIR_DOMAIN_DISK_BUS_SD:
    case VIR_DOMAIN_DISK_BUS_NONE:
    case VIR_DOMAIN_DISK_BUS_LAST:
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported disk bus '%s' with device setup"),
                       NULLSTR(virDomainDiskBusTypeToString(disk->bus)));
        return NULL;
    }

    if (disk->src->shared &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DISK_SHARE_RW))
        virBufferAddLit(&opt, ",share-rw=on");

    if (virStorageSourceGetActualType(disk->src) == VIR_STORAGE_TYPE_VHOST_USER) {
        backendAlias = qemuDomainGetVhostUserChrAlias(disk->info.alias);

        virBufferAsprintf(&opt, ",chardev=%s", backendAlias);
    } else {
        if (qemuDomainDiskGetBackendAlias(disk, qemuCaps, &backendAlias) < 0)
            return NULL;

        if (backendAlias)
            virBufferAsprintf(&opt, ",drive=%s", backendAlias);
    }

    virBufferAsprintf(&opt, ",id=%s", disk->info.alias);
    /* bootindex for floppies is configured via the fdc controller */
    if (disk->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        diskPriv->effectiveBootindex > 0)
        virBufferAsprintf(&opt, ",bootindex=%u", diskPriv->effectiveBootindex);
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKIO)) {
        if (disk->blockio.logical_block_size > 0)
            virBufferAsprintf(&opt, ",logical_block_size=%u",
                              disk->blockio.logical_block_size);
        if (disk->blockio.physical_block_size > 0)
            virBufferAsprintf(&opt, ",physical_block_size=%u",
                              disk->blockio.physical_block_size);
    }

    if (disk->wwn) {
        if (STRPREFIX(disk->wwn, "0x"))
            virBufferAsprintf(&opt, ",wwn=%s", disk->wwn);
        else
            virBufferAsprintf(&opt, ",wwn=0x%s", disk->wwn);
    }

    if (disk->rotation_rate)
        virBufferAsprintf(&opt, ",rotation_rate=%u", disk->rotation_rate);

    if (disk->vendor) {
        virBufferAddLit(&opt, ",vendor=");
        virQEMUBuildBufferEscapeComma(&opt, disk->vendor);
    }

    if (disk->product) {
        virBufferAddLit(&opt, ",product=");
        virQEMUBuildBufferEscapeComma(&opt, disk->product);
    }

    if (disk->bus == VIR_DOMAIN_DISK_BUS_USB) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_STORAGE_REMOVABLE)) {
            if (disk->removable == VIR_TRISTATE_SWITCH_ON)
                virBufferAddLit(&opt, ",removable=on");
            else
                virBufferAddLit(&opt, ",removable=off");
        }
    }

    if (qemuBuildDriveDevCacheStr(disk, &opt, qemuCaps) < 0)
        return NULL;

    qemuBuildDiskFrontendAttributes(disk, &opt);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_STORAGE_WERROR))
        qemuBuildDiskFrontendAttributeErrorPolicy(disk, &opt);

    return virBufferContentAndReset(&opt);
}

char *
qemuBuildZPCIDevStr(virDomainDeviceInfo *dev)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&buf,
                      "zpci,uid=%u,fid=%u,target=%s,id=zpci%u",
                      dev->addr.pci.zpci.uid.value,
                      dev->addr.pci.zpci.fid.value,
                      dev->alias,
                      dev->addr.pci.zpci.uid.value);

    return virBufferContentAndReset(&buf);
}

static int
qemuCommandAddZPCIDevice(virCommand *cmd,
                         virDomainDeviceInfo *dev)
{
    g_autofree char *devstr = NULL;

    virCommandAddArg(cmd, "-device");

    if (!(devstr = qemuBuildZPCIDevStr(dev)))
        return -1;

    virCommandAddArg(cmd, devstr);

    return 0;
}

static int
qemuCommandAddExtDevice(virCommand *cmd,
                        virDomainDeviceInfo *dev)
{
    if (dev->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI ||
        dev->addr.pci.extFlags == VIR_PCI_ADDRESS_EXTENSION_NONE) {
        return 0;
    }

    if (dev->addr.pci.extFlags & VIR_PCI_ADDRESS_EXTENSION_ZPCI)
        return qemuCommandAddZPCIDevice(cmd, dev);

    return 0;
}

static int
qemuBuildFloppyCommandLineControllerOptions(virCommand *cmd,
                                            const virDomainDef *def,
                                            virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) fdc_opts = VIR_BUFFER_INITIALIZER;
    bool explicitfdc = qemuDomainNeedsFDC(def);
    bool hasfloppy = false;
    char driveLetter;
    size_t i;

    virBufferAddLit(&fdc_opts, "isa-fdc,");

    for (i = 0; i < def->ndisks; i++) {
        g_autofree char *backendAlias = NULL;
        g_autofree char *backendStr = NULL;
        g_autofree char *bootindexStr = NULL;
        virDomainDiskDef *disk = def->disks[i];
        qemuDomainDiskPrivate *diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);

        if (disk->bus != VIR_DOMAIN_DISK_BUS_FDC)
            continue;

        hasfloppy = true;

        if (disk->info.addr.drive.unit)
            driveLetter = 'B';
        else
            driveLetter = 'A';

        if (diskPriv->effectiveBootindex > 0)
            bootindexStr = g_strdup_printf("bootindex%c=%u", driveLetter,
                                           diskPriv->effectiveBootindex);

        /* with -blockdev we setup the floppy device and it's backend with -device */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
            if (qemuDomainDiskGetBackendAlias(disk, qemuCaps, &backendAlias) < 0)
                return -1;

            if (backendAlias)
                backendStr = g_strdup_printf("drive%c=%s", driveLetter, backendAlias);
        }

        if (!explicitfdc) {
            if (backendStr) {
                virCommandAddArg(cmd, "-global");
                virCommandAddArgFormat(cmd, "isa-fdc.%s", backendStr);
            }

            if (bootindexStr) {
                virCommandAddArg(cmd, "-global");
                virCommandAddArgFormat(cmd, "isa-fdc.%s", bootindexStr);
            }
        } else {
            virBufferStrcat(&fdc_opts, backendStr, ",", NULL);
            virBufferStrcat(&fdc_opts, bootindexStr, ",", NULL);
        }
    }

    if (explicitfdc && hasfloppy) {
        /* Newer Q35 machine types require an explicit FDC controller */
        virBufferTrim(&fdc_opts, ",");
        virCommandAddArg(cmd, "-device");
        virCommandAddArgBuffer(cmd, &fdc_opts);
    }

    return 0;
}


static int
qemuBuildObjectCommandline(virCommand *cmd,
                           virJSONValue *objProps,
                           virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!objProps)
        return 0;

    if (qemuBuildObjectCommandlineFromJSON(&buf, objProps, qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


static int
qemuBuildBlockStorageSourceAttachDataCommandline(virCommand *cmd,
                                                 qemuBlockStorageSourceAttachData *data,
                                                 virQEMUCaps *qemuCaps)
{
    char *tmp;

    if (qemuBuildObjectCommandline(cmd, data->prmgrProps, qemuCaps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->authsecretProps, qemuCaps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->encryptsecretProps, qemuCaps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->httpcookiesecretProps, qemuCaps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->tlsKeySecretProps, qemuCaps) < 0 ||
        qemuBuildObjectCommandline(cmd, data->tlsProps, qemuCaps) < 0)
        return -1;

    if (data->driveCmd)
        virCommandAddArgList(cmd, "-drive", data->driveCmd, NULL);

    if (data->chardevCmd)
        virCommandAddArgList(cmd, "-chardev", data->chardevCmd, NULL);

    if (data->storageProps) {
        if (!(tmp = virJSONValueToString(data->storageProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", tmp, NULL);
        VIR_FREE(tmp);
    }

    if (data->storageSliceProps) {
        if (!(tmp = virJSONValueToString(data->storageSliceProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", tmp, NULL);
        VIR_FREE(tmp);
    }

    if (data->formatProps) {
        if (!(tmp = virJSONValueToString(data->formatProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", tmp, NULL);
        VIR_FREE(tmp);
    }

    return 0;
}


static int
qemuBuildDiskSourceCommandLine(virCommand *cmd,
                               virDomainDiskDef *disk,
                               virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;
    g_autoptr(virJSONValue) copyOnReadProps = NULL;
    g_autofree char *copyOnReadPropsStr = NULL;
    size_t i;

    if (virStorageSourceGetActualType(disk->src) == VIR_STORAGE_TYPE_VHOST_USER) {
        if (!(data = qemuBuildStorageSourceChainAttachPrepareChardev(disk)))
            return -1;
    } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV) &&
        !qemuDiskBusIsSD(disk->bus)) {
        if (virStorageSourceIsEmpty(disk->src))
            return 0;

        if (!(data = qemuBuildStorageSourceChainAttachPrepareBlockdev(disk->src,
                                                                      qemuCaps)))
            return -1;

        if (disk->copy_on_read == VIR_TRISTATE_SWITCH_ON &&
            !(copyOnReadProps = qemuBlockStorageGetCopyOnReadProps(disk)))
            return -1;
    } else {
        if (!(data = qemuBuildStorageSourceChainAttachPrepareDrive(disk, qemuCaps)))
            return -1;
    }

    for (i = data->nsrcdata; i > 0; i--) {
        if (qemuBuildBlockStorageSourceAttachDataCommandline(cmd,
                                                             data->srcdata[i - 1],
                                                             qemuCaps) < 0)
            return -1;
    }

    if (copyOnReadProps) {
        if (!(copyOnReadPropsStr = virJSONValueToString(copyOnReadProps, false)))
            return -1;

        virCommandAddArgList(cmd, "-blockdev", copyOnReadPropsStr, NULL);
    }

    return 0;
}


static int
qemuBuildDiskCommandLine(virCommand *cmd,
                         const virDomainDef *def,
                         virDomainDiskDef *disk,
                         virQEMUCaps *qemuCaps)
{
    g_autofree char *optstr = NULL;

    if (qemuBuildDiskSourceCommandLine(cmd, disk, qemuCaps) < 0)
        return -1;

    /* SD cards are currently instantiated via -drive if=sd, so the -device
     * part must be skipped */
    if (qemuDiskBusIsSD(disk->bus))
        return 0;

    /* floppy devices are instantiated via -drive ...,if=none and bound to the
     * controller via -global isa-fdc.driveA/B options in the pre-blockdev era */
    if (disk->bus == VIR_DOMAIN_DISK_BUS_FDC &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV))
        return 0;

    if (qemuCommandAddExtDevice(cmd, &disk->info) < 0)
        return -1;

    virCommandAddArg(cmd, "-device");

    if (!(optstr = qemuBuildDiskDeviceStr(def, disk, qemuCaps)))
        return -1;
    virCommandAddArg(cmd, optstr);

    return 0;
}


static int
qemuBuildDisksCommandLine(virCommand *cmd,
                          const virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    size_t i;
    bool blockdev = virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV);

    /* If we want to express the floppy drives via -device, the controller needs
     * to be instantiated prior to that */
    if (blockdev &&
        qemuBuildFloppyCommandLineControllerOptions(cmd, def, qemuCaps) < 0)
        return -1;

    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDef *disk = def->disks[i];

        /* transient disks with shared backing image will be hotplugged after
         * the VM is started */
        if (disk->transient &&
            disk->transientShareBacking == VIR_TRISTATE_BOOL_YES)
            continue;

        if (qemuBuildDiskCommandLine(cmd, def, disk, qemuCaps) < 0)
            return -1;
    }

    if (!blockdev &&
        qemuBuildFloppyCommandLineControllerOptions(cmd, def, qemuCaps) < 0)
        return -1;

    return 0;
}


static int
qemuBuildVHostUserFsCommandLine(virCommand *cmd,
                                virDomainFSDef *fs,
                                const virDomainDef *def,
                                qemuDomainObjPrivate *priv)
{
    g_autofree char *chardev_alias = NULL;
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;

    chardev_alias = qemuDomainGetVhostUserChrAlias(fs->info.alias);

    virCommandAddArg(cmd, "-chardev");
    virBufferAddLit(&opt, "socket");
    virBufferAsprintf(&opt, ",id=%s", chardev_alias);
    virBufferAddLit(&opt, ",path=");
    virQEMUBuildBufferEscapeComma(&opt, QEMU_DOMAIN_FS_PRIVATE(fs)->vhostuser_fs_sock);
    virCommandAddArgBuffer(cmd, &opt);

    virCommandAddArg(cmd, "-device");

    if (qemuBuildVirtioDevStr(&opt, "vhost-user-fs", priv->qemuCaps,
                              VIR_DOMAIN_DEVICE_FS, fs) < 0)
        return -1;

    virBufferAsprintf(&opt, ",chardev=%s", chardev_alias);
    if (fs->queue_size)
        virBufferAsprintf(&opt, ",queue-size=%llu", fs->queue_size);
    virBufferAddLit(&opt, ",tag=");
    virQEMUBuildBufferEscapeComma(&opt, fs->dst);
    qemuBuildVirtioOptionsStr(&opt, fs->virtio);

    if (fs->info.bootIndex)
        virBufferAsprintf(&opt, ",bootindex=%u", fs->info.bootIndex);

    if (qemuBuildDeviceAddressStr(&opt, def, &fs->info) < 0)
        return -1;

    virCommandAddArgBuffer(cmd, &opt);
    return 0;
}


static char *
qemuBuildFSStr(virDomainFSDef *fs)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;
    const char *wrpolicy = virDomainFSWrpolicyTypeToString(fs->wrpolicy);

    if (fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_PATH ||
        fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_DEFAULT) {
        virBufferAddLit(&opt, "local");
        if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_MAPPED) {
            virBufferAddLit(&opt, ",security_model=mapped");
        } else if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH) {
            virBufferAddLit(&opt, ",security_model=passthrough");
        } else if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_SQUASH) {
            virBufferAddLit(&opt, ",security_model=none");
        }
        if (fs->multidevs == VIR_DOMAIN_FS_MULTIDEVS_REMAP) {
            virBufferAddLit(&opt, ",multidevs=remap");
        } else if (fs->multidevs == VIR_DOMAIN_FS_MULTIDEVS_FORBID) {
            virBufferAddLit(&opt, ",multidevs=forbid");
        } else if (fs->multidevs == VIR_DOMAIN_FS_MULTIDEVS_WARN) {
            virBufferAddLit(&opt, ",multidevs=warn");
        }
        if (fs->fmode) {
            virBufferAsprintf(&opt, ",fmode=%04o", fs->fmode);
        }
        if (fs->dmode) {
            virBufferAsprintf(&opt, ",dmode=%04o", fs->dmode);
        }
    } else if (fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_HANDLE) {
        /* removed since qemu 4.0.0 see v3.1.0-29-g93aee84f57 */
        virBufferAddLit(&opt, "handle");
    }

    if (fs->wrpolicy)
        virBufferAsprintf(&opt, ",writeout=%s", wrpolicy);

    virBufferAsprintf(&opt, ",id=%s%s", QEMU_FSDEV_HOST_PREFIX, fs->info.alias);
    virBufferAddLit(&opt, ",path=");
    virQEMUBuildBufferEscapeComma(&opt, fs->src->path);

    if (fs->readonly)
        virBufferAddLit(&opt, ",readonly");

    return virBufferContentAndReset(&opt);
}


static char *
qemuBuildFSDevStr(const virDomainDef *def,
                  virDomainFSDef *fs,
                  virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;

    if (qemuBuildVirtioDevStr(&opt, "virtio-9p", qemuCaps,
                              VIR_DOMAIN_DEVICE_FS, fs) < 0)
        return NULL;

    virBufferAsprintf(&opt, ",id=%s", fs->info.alias);
    virBufferAsprintf(&opt, ",fsdev=%s%s",
                      QEMU_FSDEV_HOST_PREFIX, fs->info.alias);
    virBufferAddLit(&opt, ",mount_tag=");
    virQEMUBuildBufferEscapeComma(&opt, fs->dst);

    qemuBuildVirtioOptionsStr(&opt, fs->virtio);

    if (qemuBuildDeviceAddressStr(&opt, def, &fs->info) < 0)
        return NULL;

    return virBufferContentAndReset(&opt);
}


static int
qemuBuildFSDevCommandLine(virCommand *cmd,
                          virDomainFSDef *fs,
                          const virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    g_autofree char *fsdevstr = NULL;
    g_autofree char *devicestr = NULL;

    virCommandAddArg(cmd, "-fsdev");
    if (!(fsdevstr = qemuBuildFSStr(fs)))
        return -1;
    virCommandAddArg(cmd, fsdevstr);

    if (qemuCommandAddExtDevice(cmd, &fs->info) < 0)
        return -1;

    virCommandAddArg(cmd, "-device");
    if (!(devicestr = qemuBuildFSDevStr(def, fs, qemuCaps)))
        return -1;
    virCommandAddArg(cmd, devicestr);

    return 0;
}


static int
qemuBuildFilesystemCommandLine(virCommand *cmd,
                               const virDomainDef *def,
                               virQEMUCaps *qemuCaps,
                               qemuDomainObjPrivate *priv)
{
    size_t i;

    for (i = 0; i < def->nfss; i++) {
        switch ((virDomainFSDriverType) def->fss[i]->fsdriver) {
        case VIR_DOMAIN_FS_DRIVER_TYPE_DEFAULT:
        case VIR_DOMAIN_FS_DRIVER_TYPE_PATH:
        case VIR_DOMAIN_FS_DRIVER_TYPE_HANDLE:
            /* these drivers are handled by virtio-9p-pci */
            if (qemuBuildFSDevCommandLine(cmd, def->fss[i], def, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_FS_DRIVER_TYPE_VIRTIOFS:
            /* vhost-user-fs-pci */
            if (qemuBuildVHostUserFsCommandLine(cmd, def->fss[i], def, priv) < 0)
                return -1;
            break;

        case VIR_DOMAIN_FS_DRIVER_TYPE_LOOP:
        case VIR_DOMAIN_FS_DRIVER_TYPE_NBD:
        case VIR_DOMAIN_FS_DRIVER_TYPE_PLOOP:
        case VIR_DOMAIN_FS_DRIVER_TYPE_LAST:
            break;
        }
    }

    return 0;
}


static int
qemuControllerModelUSBToCaps(int model)
{
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI:
        return QEMU_CAPS_PIIX3_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX4_UHCI:
        return QEMU_CAPS_PIIX4_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_EHCI:
        return QEMU_CAPS_USB_EHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3:
        return QEMU_CAPS_ICH9_USB_EHCI1;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_VT82C686B_UHCI:
        return QEMU_CAPS_VT82C686B_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PCI_OHCI:
        return QEMU_CAPS_PCI_OHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI:
        return QEMU_CAPS_NEC_USB_XHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI:
        return QEMU_CAPS_DEVICE_QEMU_XHCI;
    default:
        return -1;
    }
}


static const char *
qemuBuildUSBControllerFindMasterAlias(const virDomainDef *domainDef,
                                      const virDomainControllerDef *def)
{
    size_t i;

    for (i = 0; i < domainDef->ncontrollers; i++) {
        const virDomainControllerDef *tmp = domainDef->controllers[i];

        if (tmp->type != VIR_DOMAIN_CONTROLLER_TYPE_USB)
            continue;

        if (tmp->idx != def->idx)
            continue;

        if (tmp->info.mastertype == VIR_DOMAIN_CONTROLLER_MASTER_USB)
            continue;

        return tmp->info.alias;
    }

    return NULL;
}


static int
qemuBuildUSBControllerDevStr(const virDomainDef *domainDef,
                             virDomainControllerDef *def,
                             virQEMUCaps *qemuCaps,
                             virBuffer *buf)
{
    const char *smodel;
    int model, flags;

    model = def->model;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       "%s", _("no model provided for USB controller"));
        return -1;
    }

    smodel = qemuControllerModelUSBTypeToString(model);
    flags = qemuControllerModelUSBToCaps(model);

    if (flags == -1 || !virQEMUCapsGet(qemuCaps, flags)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("%s not supported in this QEMU binary"), smodel);
        return -1;
    }

    virBufferAsprintf(buf, "%s", smodel);

    if (def->opts.usbopts.ports != -1) {
        if ((model != VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI ||
             !virQEMUCapsGet(qemuCaps, QEMU_CAPS_NEC_USB_XHCI_PORTS)) &&
            model != VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("usb controller type %s doesn't support 'ports' "
                             "with this QEMU binary"), smodel);
            return -1;
        }

        virBufferAsprintf(buf, ",p2=%d,p3=%d",
                          def->opts.usbopts.ports, def->opts.usbopts.ports);
    }

    if (def->info.mastertype == VIR_DOMAIN_CONTROLLER_MASTER_USB) {
        const char *masterbus;

        if (!(masterbus = qemuBuildUSBControllerFindMasterAlias(domainDef, def))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("masterbus not found"));
            return -1;
        }
        virBufferAsprintf(buf, ",masterbus=%s.0,firstport=%d",
                          masterbus, def->info.master.usb.startport);
    } else {
        virBufferAsprintf(buf, ",id=%s", def->info.alias);
    }

    return 0;
}


/**
 * qemuBuildControllerDevStr:
 * @domainDef: domain definition
 * @def: controller definition
 * @qemuCaps: QEMU binary capabilities
 * @devstr: device string
 * @nusbcontroller: number of USB controllers
 *
 * Turn @def into a description of the controller that QEMU will understand,
 * to be used either on the command line or through the monitor.
 *
 * The description will be returned in @devstr and can be NULL, eg. when
 * passing in one of the built-in controllers. The returned string must be
 * freed by the caller.
 *
 * The number pointed to by @nusbcontroller will be increased by one every
 * time the description for a USB controller has been generated successfully.
 *
 * Returns: 0 on success, <0 on failure
 */
int
qemuBuildControllerDevStr(const virDomainDef *domainDef,
                          virDomainControllerDef *def,
                          virQEMUCaps *qemuCaps,
                          char **devstr)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    *devstr = NULL;

    switch ((virDomainControllerType)def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        switch ((virDomainControllerModelSCSI) def->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_TRANSITIONAL:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_NON_TRANSITIONAL:
            if (qemuBuildVirtioDevStr(&buf, "virtio-scsi", qemuCaps,
                                      VIR_DOMAIN_DEVICE_CONTROLLER, def) < 0) {
                return -1;
            }

            if (def->iothread) {
                virBufferAsprintf(&buf, ",iothread=iothread%u",
                                  def->iothread);
            }

            qemuBuildVirtioOptionsStr(&buf, def->virtio);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
            virBufferAddLit(&buf, "lsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
            virBufferAddLit(&buf, "spapr-vscsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1068:
            virBufferAddLit(&buf, "mptsas1068");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
            virBufferAddLit(&buf, "megasas");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VMPVSCSI:
            virBufferAddLit(&buf, "pvscsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AM53C974:
            virBufferAddLit(&buf, "am53c974");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DC390:
            virBufferAddLit(&buf, "dc-390");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AUTO:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_BUSLOGIC:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_NCR53C90: /* It is built-in dev */
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported controller model: %s"),
                           virDomainControllerModelSCSITypeToString(def->model));
            return -1;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DEFAULT:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected SCSI controller model %d"),
                           def->model);
            return -1;
        }
        virBufferAsprintf(&buf, ",id=%s", def->info.alias);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
        if (qemuBuildVirtioDevStr(&buf, "virtio-serial", qemuCaps,
                                  VIR_DOMAIN_DEVICE_CONTROLLER, def) < 0) {
            return -1;
        }

        virBufferAsprintf(&buf, ",id=%s", def->info.alias);
        if (def->opts.vioserial.ports != -1) {
            virBufferAsprintf(&buf, ",max_ports=%d",
                              def->opts.vioserial.ports);
        }
        if (def->opts.vioserial.vectors != -1) {
            virBufferAsprintf(&buf, ",vectors=%d",
                              def->opts.vioserial.vectors);
        }
        qemuBuildVirtioOptionsStr(&buf, def->virtio);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
        virBufferAsprintf(&buf, "usb-ccid,id=%s", def->info.alias);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
        virBufferAsprintf(&buf, "ahci,id=%s", def->info.alias);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_USB:
        if (qemuBuildUSBControllerDevStr(domainDef, def, qemuCaps, &buf) == -1)
            return -1;

        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_PCI: {
        const virDomainPCIControllerOpts *pciopts = &def->opts.pciopts;
        const char *modelName = virDomainControllerPCIModelNameTypeToString(pciopts->modelName);

        /* Skip the implicit PHB for pSeries guests */
        if (def->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT &&
            pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE &&
            pciopts->targetIndex == 0) {
            return 0;
        }

        if (!modelName) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown virDomainControllerPCIModelName value: %d"),
                           pciopts->modelName);
            return -1;
        }

        switch ((virDomainControllerModelPCI) def->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
            virBufferAsprintf(&buf, "%s,chassis_nr=%d,id=%s",
                              modelName, pciopts->chassisNr,
                              def->info.alias);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
            virBufferAsprintf(&buf, "%s,bus_nr=%d,id=%s",
                              modelName, pciopts->busNr,
                              def->info.alias);
            if (pciopts->numaNode != -1) {
                virBufferAsprintf(&buf, ",numa_node=%d",
                                  pciopts->numaNode);
            }
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
            virBufferAsprintf(&buf, "%s,id=%s", modelName, def->info.alias);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
            virBufferAsprintf(&buf, "%s,port=0x%x,chassis=%d,id=%s",
                              modelName, pciopts->port,
                              pciopts->chassis, def->info.alias);
            if (pciopts->hotplug != VIR_TRISTATE_SWITCH_ABSENT) {
                virBufferAsprintf(&buf, ",hotplug=%s",
                                  virTristateSwitchTypeToString(pciopts->hotplug));
            }
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
            virBufferAsprintf(&buf, "%s,index=%d,id=%s",
                              modelName, pciopts->targetIndex,
                              def->info.alias);

            if (pciopts->numaNode != -1)
                virBufferAsprintf(&buf, ",numa_node=%d", pciopts->numaNode);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Unsupported PCI Express root controller"));
            return -1;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unexpected PCI controller model %d"),
                           def->model);
            return -1;
        }
        break;
    }

    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
    case VIR_DOMAIN_CONTROLLER_TYPE_XENBUS:
    case VIR_DOMAIN_CONTROLLER_TYPE_ISA:
    case VIR_DOMAIN_CONTROLLER_TYPE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported controller type: %s"),
                       virDomainControllerTypeToString(def->type));
        return -1;
    }

    if (def->queues)
        virBufferAsprintf(&buf, ",num_queues=%u", def->queues);

    if (def->cmd_per_lun)
        virBufferAsprintf(&buf, ",cmd_per_lun=%u", def->cmd_per_lun);

    if (def->max_sectors)
        virBufferAsprintf(&buf, ",max_sectors=%u", def->max_sectors);

    qemuBuildIoEventFdStr(&buf, def->ioeventfd, qemuCaps);

    if (qemuBuildDeviceAddressStr(&buf, domainDef, &def->info) < 0)
        return -1;

    *devstr = virBufferContentAndReset(&buf);
    return 0;
}


static bool
qemuBuildDomainForbidLegacyUSBController(const virDomainDef *def)
{
    if (qemuDomainIsQ35(def) ||
        qemuDomainIsARMVirt(def) ||
        qemuDomainIsRISCVVirt(def))
        return true;

    return false;
}


static int
qemuBuildLegacyUSBControllerCommandLine(virCommand *cmd,
                                        const virDomainDef *def)
{
    size_t i;
    size_t nlegacy = 0;
    size_t nusb = 0;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDef *cont = def->controllers[i];

        if (cont->type != VIR_DOMAIN_CONTROLLER_TYPE_USB)
            continue;

        /* If we have mode='none', there are no other USB controllers */
        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE)
            return 0;

        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT)
            nlegacy++;
        else
            nusb++;
    }

    if (nlegacy > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Multiple legacy USB controllers are "
                         "not supported"));
        return -1;
    }

    if (nusb == 0 &&
        !qemuBuildDomainForbidLegacyUSBController(def) &&
        !ARCH_IS_S390(def->os.arch)) {
        /* We haven't added any USB controller yet, but we haven't been asked
         * not to add one either. Add a legacy USB controller, unless we're
         * creating a kind of guest we want to keep legacy-free */
        virCommandAddArg(cmd, "-usb");
    }

    return 0;
}


/**
 * qemuBuildSkipController:
 * @controller: Controller to check
 * @def: Domain definition
 *
 * Returns true if this controller can be skipped for command line
 * generation or device validation.
 */
static bool
qemuBuildSkipController(const virDomainControllerDef *controller,
                        const virDomainDef *def)
{
    /* skip pcie-root */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
        controller->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT)
        return true;

    /* Skip pci-root, except for pSeries guests (which actually
     * support more than one PCI Host Bridge per guest) */
    if (!qemuDomainIsPSeries(def) &&
        controller->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
        controller->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT)
        return true;

    /* first SATA controller on Q35 machines is implicit */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SATA &&
        controller->idx == 0 && qemuDomainIsQ35(def))
        return true;

    /* first IDE controller is implicit on various machines */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_IDE &&
        controller->idx == 0 && qemuDomainHasBuiltinIDE(def))
        return true;

    /* first ESP SCSI controller is implicit on certain machine types */
    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI &&
        controller->idx == 0 &&
        controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_NCR53C90 &&
        qemuDomainHasBuiltinESP(def)) {
        return true;
    }

    return false;
}


static int
qemuBuildControllersByTypeCommandLine(virCommand *cmd,
                                      const virDomainDef *def,
                                      virQEMUCaps *qemuCaps,
                                      virDomainControllerType type)
{
    size_t i;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDef *cont = def->controllers[i];
        g_autofree char *devstr = NULL;

        if (cont->type != type)
            continue;

        if (qemuBuildSkipController(cont, def))
            continue;

        /* skip USB controllers with type none.*/
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE) {
            continue;
        }

        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT &&
            !qemuBuildDomainForbidLegacyUSBController(def)) {

            /* An appropriate default USB controller model should already
             * have been selected in qemuDomainDeviceDefPostParse(); if
             * we still have no model by now, we have to fall back to the
             * legacy USB controller.
             *
             * Note that we *don't* want to end up with the legacy USB
             * controller for q35 and virt machines, so we go ahead and
             * fail in qemuBuildControllerDevStr(); on the other hand,
             * for s390 machines we want to ignore any USB controller
             * (see 548ba43028 for the full story), so we skip
             * qemuBuildControllerDevStr() but we don't ultimately end
             * up adding the legacy USB controller */
            continue;
        }

        if (qemuBuildControllerDevStr(def, cont, qemuCaps, &devstr) < 0)
            return -1;

        if (devstr) {
            if (qemuCommandAddExtDevice(cmd, &cont->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");
            virCommandAddArg(cmd, devstr);
        }
    }

    return 0;
}


static int
qemuBuildControllersCommandLine(virCommand *cmd,
                                const virDomainDef *def,
                                virQEMUCaps *qemuCaps)
{
    size_t i;
    int contOrder[] = {
        /*
         * List of controller types that we add commandline args for,
         * *in the order we want to add them*.
         *
         * The floppy controller is implicit on PIIX4 and older Q35
         * machines. For newer Q35 machines it is added out of the
         * controllers loop, after the floppy drives.
         *
         * We don't add PCI/PCIe root controller either, because it's
         * implicit, but we do add PCI bridges and other PCI
         * controllers, so we leave that in to check each
         * one. Likewise, we don't do anything for the primary IDE
         * controller on an i440fx machine or primary SATA on q35, but
         * we do add those beyond these two exceptions.
         *
         * CCID controllers are formatted separately after USB hubs,
         * because they go on the USB bus.
         */
        VIR_DOMAIN_CONTROLLER_TYPE_PCI,
        VIR_DOMAIN_CONTROLLER_TYPE_USB,
        VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
        VIR_DOMAIN_CONTROLLER_TYPE_IDE,
        VIR_DOMAIN_CONTROLLER_TYPE_SATA,
        VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL,
    };

    for (i = 0; i < G_N_ELEMENTS(contOrder); i++) {
        if (qemuBuildControllersByTypeCommandLine(cmd, def, qemuCaps, contOrder[i]) < 0)
            return -1;
    }

    if (qemuBuildLegacyUSBControllerCommandLine(cmd, def) < 0)
        return -1;

    return 0;
}


static int
qemuBuildMemoryBackendPropsShare(virJSONValue *props,
                                 virDomainMemoryAccess memAccess)
{
    switch (memAccess) {
    case VIR_DOMAIN_MEMORY_ACCESS_SHARED:
        return virJSONValueObjectAdd(props, "b:share", true, NULL);

    case VIR_DOMAIN_MEMORY_ACCESS_PRIVATE:
        return virJSONValueObjectAdd(props, "b:share", false, NULL);

    case VIR_DOMAIN_MEMORY_ACCESS_DEFAULT:
    case VIR_DOMAIN_MEMORY_ACCESS_LAST:
        break;
    }

    return 0;
}


static int
qemuBuildMemoryGetDefaultPagesize(virQEMUDriverConfig *cfg,
                                  unsigned long long *pagesize)
{
    virHugeTLBFS *p;

    if (!cfg->nhugetlbfs) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("hugetlbfs filesystem is not mounted "
                               "or disabled by administrator config"));
        return -1;
    }

    if (!(p = virFileGetDefaultHugepage(cfg->hugetlbfs, cfg->nhugetlbfs)))
        p = &cfg->hugetlbfs[0];

    *pagesize = p->size;
    return 0;
}


/**
 * qemuBuildMemoryBackendProps:
 * @backendProps: [out] constructed object
 * @alias: alias of the device
 * @cfg: qemu driver config object
 * @priv: pointer to domain private object
 * @def: domain definition object
 * @mem: memory definition object
 * @force: forcibly use one of the backends
 *
 * Creates a configuration object that represents memory backend of given guest
 * NUMA node (domain @def and @mem). Use @priv->autoNodeset to fine tune the
 * placement of the memory on the host NUMA nodes.
 *
 * By default, if no memory-backend-* object is necessary to fulfil the guest
 * configuration value of 1 is returned. This behaviour can be suppressed by
 * setting @force to true in which case 0 would be returned.
 *
 * Then, if one of the three memory-backend-* should be used, the @priv->qemuCaps
 * is consulted to check if qemu does support it.
 *
 * Returns: 0 on success,
 *          1 on success and if there's no need to use memory-backend-*
 *         -1 on error.
 */
int
qemuBuildMemoryBackendProps(virJSONValue **backendProps,
                            const char *alias,
                            virQEMUDriverConfig *cfg,
                            qemuDomainObjPrivate *priv,
                            const virDomainDef *def,
                            const virDomainMemoryDef *mem,
                            bool force,
                            bool systemMemory)
{
    const char *backendType = "memory-backend-file";
    virDomainNumatuneMemMode mode;
    const long system_page_size = virGetSystemPageSizeKB();
    virDomainMemoryAccess memAccess = mem->access;
    size_t i;
    g_autofree char *memPath = NULL;
    bool prealloc = false;
    virBitmap *nodemask = NULL;
    int rc;
    g_autoptr(virJSONValue) props = NULL;
    bool nodeSpecified = virDomainNumatuneNodeSpecified(def->numa, mem->targetNode);
    unsigned long long pagesize = mem->pagesize;
    bool needHugepage = !!pagesize;
    bool useHugepage = !!pagesize;
    int discard = mem->discard;
    bool disableCanonicalPath = false;

    /* Disabling canonical path is required for migration compatibility of
     * system memory objects, see below */

    /* The difference between @needHugepage and @useHugepage is that the latter
     * is true whenever huge page is defined for the current memory cell.
     * Either directly, or transitively via global domain huge pages. The
     * former is true whenever "memory-backend-file" must be used to satisfy
     * @useHugepage. */

    *backendProps = NULL;

    if (mem->targetNode >= 0) {
        /* memory devices could provide a invalid guest node */
        if (mem->targetNode >= virDomainNumaGetNodeCount(def->numa)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("can't add memory backend for guest node '%d' as "
                             "the guest has only '%zu' NUMA nodes configured"),
                           mem->targetNode, virDomainNumaGetNodeCount(def->numa));
            return -1;
        }

        if (memAccess == VIR_DOMAIN_MEMORY_ACCESS_DEFAULT)
            memAccess = virDomainNumaGetNodeMemoryAccessMode(def->numa, mem->targetNode);

        if (discard == VIR_TRISTATE_BOOL_ABSENT)
            discard = virDomainNumaGetNodeDiscard(def->numa, mem->targetNode);
    }

    if (memAccess == VIR_DOMAIN_MEMORY_ACCESS_DEFAULT)
        memAccess = def->mem.access;

    if (discard == VIR_TRISTATE_BOOL_ABSENT)
        discard = def->mem.discard;

    if (def->mem.allocation == VIR_DOMAIN_MEMORY_ALLOCATION_IMMEDIATE)
        prealloc = true;

    if (virDomainNumatuneGetMode(def->numa, mem->targetNode, &mode) < 0 &&
        virDomainNumatuneGetMode(def->numa, -1, &mode) < 0)
        mode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;

    if (pagesize == 0) {
        virDomainHugePage *master_hugepage = NULL;
        virDomainHugePage *hugepage = NULL;
        bool thisHugepage = false;

        /* Find the huge page size we want to use */
        for (i = 0; i < def->mem.nhugepages; i++) {
            hugepage = &def->mem.hugepages[i];

            if (!hugepage->nodemask) {
                master_hugepage = hugepage;
                continue;
            }

            /* just find the master hugepage in case we don't use NUMA */
            if (mem->targetNode < 0)
                continue;

            if (virBitmapGetBit(hugepage->nodemask, mem->targetNode,
                                &thisHugepage) < 0) {
                /* Ignore this error. It's not an error after all. Well,
                 * the nodemask for this <page/> can contain lower NUMA
                 * nodes than we are querying in here. */
                continue;
            }

            if (thisHugepage) {
                /* Hooray, we've found the page size */
                needHugepage = true;
                break;
            }
        }

        if (i == def->mem.nhugepages) {
            /* We have not found specific huge page to be used with this
             * NUMA node. Use the generic setting then (<page/> without any
             * @nodemask) if possible. */
            hugepage = master_hugepage;
        }

        if (hugepage) {
            pagesize = hugepage->size;
            useHugepage = true;
        }
    }

    if (pagesize == system_page_size) {
        /* However, if user specified to use "huge" page
         * of regular system page size, it's as if they
         * hasn't specified any huge pages at all. */
        pagesize = 0;
        needHugepage = false;
        useHugepage = false;
    } else if (useHugepage && pagesize == 0) {
        if (qemuBuildMemoryGetDefaultPagesize(cfg, &pagesize) < 0)
            return -1;
    }

    props = virJSONValueNewObject();

    if (!mem->nvdimmPath &&
        def->mem.source == VIR_DOMAIN_MEMORY_SOURCE_MEMFD) {
        backendType = "memory-backend-memfd";

        if (useHugepage) {
            if (virJSONValueObjectAdd(props, "b:hugetlb", useHugepage, NULL) < 0 ||
                virJSONValueObjectAdd(props, "U:hugetlbsize", pagesize << 10, NULL) < 0) {
                return -1;
            }

            prealloc = true;
        }

        if (qemuBuildMemoryBackendPropsShare(props, memAccess) < 0)
            return -1;

        if (systemMemory)
            disableCanonicalPath = true;

    } else if (useHugepage || mem->nvdimmPath || memAccess ||
        def->mem.source == VIR_DOMAIN_MEMORY_SOURCE_FILE) {

        if (mem->nvdimmPath) {
            memPath = g_strdup(mem->nvdimmPath);
            /* If the NVDIMM is a real device then there's nothing to prealloc.
             * If anything, we would be only wearing off the device.
             * Similarly, virtio-pmem-pci doesn't need prealloc either. */
            if (!mem->nvdimmPmem && mem->model != VIR_DOMAIN_MEMORY_MODEL_VIRTIO_PMEM)
                prealloc = true;
        } else if (useHugepage) {
            if (qemuGetDomainHupageMemPath(priv->driver, def, pagesize, &memPath) < 0)
                return -1;
            prealloc = true;
        } else {
            /* We can have both pagesize and mem source. If that's the case,
             * prefer hugepages as those are more specific. */
            if (qemuGetMemoryBackingPath(priv->driver, def, mem->info.alias, &memPath) < 0)
                return -1;
        }

        if (virJSONValueObjectAdd(props,
                                  "s:mem-path", memPath,
                                  NULL) < 0)
            return -1;

        if (!mem->nvdimmPath &&
            discard == VIR_TRISTATE_BOOL_YES) {
            if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE_DISCARD)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("this QEMU doesn't support memory discard"));
                return -1;
            }

            if (virJSONValueObjectAdd(props,
                                      "B:discard-data", true,
                                      NULL) < 0)
                return -1;
        }

        if (qemuBuildMemoryBackendPropsShare(props, memAccess) < 0)
            return -1;

        if (systemMemory)
            disableCanonicalPath = true;

    } else {
        backendType = "memory-backend-ram";
    }

    /* This is a terrible hack, but unfortunately there is no better way.
     * The replacement for '-m X' argument is not simple '-machine
     * memory-backend' and '-object memory-backend-*,size=X' (which was the
     * idea). This is because of create_default_memdev() in QEMU sets
     * 'x-use-canonical-path-for-ramblock-id' attribute to false and is
     * documented in QEMU in qemu-options.hx under 'memory-backend'. Note
     * that QEMU considers 'x-use-canonical-path-for-ramblock-id' stable
     * and supported despite the 'x-' prefix.
     * See QEMU commit 8db0b20415c129cf5e577a593a4a0372d90b7cc9.
     */
    if (disableCanonicalPath &&
        virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_X_USE_CANONICAL_PATH_FOR_RAMBLOCK_ID) &&
        virJSONValueObjectAdd(props, "b:x-use-canonical-path-for-ramblock-id", false, NULL) < 0)
        return -1;

    if (!priv->memPrealloc &&
        virJSONValueObjectAdd(props, "B:prealloc", prealloc, NULL) < 0)
        return -1;

    if (virJSONValueObjectAdd(props, "U:size", mem->size * 1024, NULL) < 0)
        return -1;

    if (mem->alignsize) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE_ALIGN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("nvdimm align property is not available "
                             "with this QEMU binary"));
            return -1;
        }
        if (virJSONValueObjectAdd(props, "U:align", mem->alignsize * 1024, NULL) < 0)
            return -1;
    }

    if (mem->nvdimmPmem) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE_PMEM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("nvdimm pmem property is not available "
                             "with this QEMU binary"));
            return -1;
        }
        if (virJSONValueObjectAdd(props, "b:pmem", true, NULL) < 0)
            return -1;
    }

    if (mem->sourceNodes) {
        nodemask = mem->sourceNodes;
    } else {
        if (virDomainNumatuneMaybeGetNodeset(def->numa, priv->autoNodeset,
                                             &nodemask, mem->targetNode) < 0)
            return -1;
    }

    /* If mode is "restrictive", we should only use cgroups setting allowed memory
     * nodes, and skip passing the host-nodes and policy parameters to QEMU command
     * line which means we will use system default memory policy. */
    if (nodemask && mode != VIR_DOMAIN_NUMATUNE_MEM_RESTRICTIVE) {
        if (!virNumaNodesetIsAvailable(nodemask))
            return -1;
        if (virJSONValueObjectAdd(props,
                                  "m:host-nodes", nodemask,
                                  "S:policy", qemuNumaPolicyTypeToString(mode),
                                  NULL) < 0)
            return -1;
    }

    /* If none of the following is requested... */
    if (!needHugepage && !mem->sourceNodes && !nodeSpecified &&
        !mem->nvdimmPath &&
        memAccess == VIR_DOMAIN_MEMORY_ACCESS_DEFAULT &&
        def->mem.source != VIR_DOMAIN_MEMORY_SOURCE_FILE &&
        def->mem.source != VIR_DOMAIN_MEMORY_SOURCE_MEMFD &&
        !force) {
        /* report back that using the new backend is not necessary
         * to achieve the desired configuration */
        rc = 1;
    } else {
        /* otherwise check the required capability */
        if (STREQ(backendType, "memory-backend-file") &&
            !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the "
                             "memory-backend-file object"));
            return -1;
        } else if (STREQ(backendType, "memory-backend-ram") &&
                   !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_RAM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the "
                             "memory-backend-ram object"));
            return -1;
        } else if (STREQ(backendType, "memory-backend-memfd") &&
                   !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_MEMORY_MEMFD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the "
                             "memory-backend-memfd object"));
            return -1;
        }

        rc = 0;
    }

    if (virJSONValueObjectPrependString(props, "id", alias) < 0 ||
        virJSONValueObjectPrependString(props, "qom-type", backendType) < 0)
        return -1;

    *backendProps = g_steal_pointer(&props);

    return rc;
}


static int
qemuBuildMemoryCellBackendStr(virDomainDef *def,
                              virQEMUDriverConfig *cfg,
                              size_t cell,
                              qemuDomainObjPrivate *priv,
                              virBuffer *buf)
{
    g_autoptr(virJSONValue) props = NULL;
    g_autofree char *alias = NULL;
    int rc;
    virDomainMemoryDef mem = { 0 };
    unsigned long long memsize = virDomainNumaGetNodeMemorySize(def->numa,
                                                                cell);

    alias = g_strdup_printf("ram-node%zu", cell);

    mem.size = memsize;
    mem.targetNode = cell;
    mem.info.alias = alias;

    if ((rc = qemuBuildMemoryBackendProps(&props, alias, cfg,
                                          priv, def, &mem, false, false)) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(buf, props, priv->qemuCaps) < 0)
        return -1;

    return rc;
}


static int
qemuBuildMemoryDimmBackendStr(virBuffer *buf,
                              virDomainMemoryDef *mem,
                              virDomainDef *def,
                              virQEMUDriverConfig *cfg,
                              qemuDomainObjPrivate *priv)
{
    g_autoptr(virJSONValue) props = NULL;
    g_autofree char *alias = NULL;

    if (!mem->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("memory device alias is not assigned"));
        return -1;
    }

    alias = g_strdup_printf("mem%s", mem->info.alias);

    if (qemuBuildMemoryBackendProps(&props, alias, cfg,
                                    priv, def, mem, true, false) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(buf, props, priv->qemuCaps) < 0)
        return -1;

    return 0;
}


char *
qemuBuildMemoryDeviceStr(const virDomainDef *def,
                         virDomainMemoryDef *mem,
                         virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *device = NULL;

    if (!mem->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing alias for memory device"));
        return NULL;
    }

    switch (mem->model) {
    case VIR_DOMAIN_MEMORY_MODEL_DIMM:
        device = "pc-dimm";
        break;
    case VIR_DOMAIN_MEMORY_MODEL_NVDIMM:
        device = "nvdimm";
        break;

    case VIR_DOMAIN_MEMORY_MODEL_VIRTIO_PMEM:
        device = "virtio-pmem-pci";
        break;

    case VIR_DOMAIN_MEMORY_MODEL_NONE:
    case VIR_DOMAIN_MEMORY_MODEL_LAST:
    default:
        virReportEnumRangeError(virDomainMemoryModel, mem->model);
        return NULL;
        break;
    }

    virBufferAsprintf(&buf, "%s,", device);

    if (mem->targetNode >= 0)
        virBufferAsprintf(&buf, "node=%d,", mem->targetNode);

    if (mem->labelsize)
        virBufferAsprintf(&buf, "label-size=%llu,", mem->labelsize * 1024);

    if (mem->uuid) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        virUUIDFormat(mem->uuid, uuidstr);
        virBufferAsprintf(&buf, "uuid=%s,", uuidstr);
    }

    if (mem->readonly) {
        virBufferAddLit(&buf, "unarmed=on,");
    }

    virBufferAsprintf(&buf, "memdev=mem%s,id=%s",
                      mem->info.alias, mem->info.alias);

    if (qemuBuildDeviceAddressStr(&buf, def, &mem->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildLegacyNicStr(virDomainNetDef *net)
{
    char *str;
    char macaddr[VIR_MAC_STRING_BUFLEN];
    const char *netmodel = virDomainNetGetModelString(net);

    str = g_strdup_printf("nic,macaddr=%s,netdev=host%s%s%s%s%s",
                          virMacAddrFormat(&net->mac, macaddr),
                          net->info.alias,
                          netmodel ? ",model=" : "",
                          NULLSTR_EMPTY(netmodel),
                          (net->info.alias ? ",id=" : ""),
                          NULLSTR_EMPTY(net->info.alias));
    return str;
}


char *
qemuBuildNicDevStr(virDomainDef *def,
                   virDomainNetDef *net,
                   unsigned int bootindex,
                   size_t vhostfdSize,
                   virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    bool usingVirtio = false;
    char macaddr[VIR_MAC_STRING_BUFLEN];

    if (virDomainNetIsVirtioModel(net)) {
        if (qemuBuildVirtioDevStr(&buf, "virtio-net", qemuCaps,
                                  VIR_DOMAIN_DEVICE_NET, net) < 0) {
            return NULL;
        }

        usingVirtio = true;
    } else {
        virBufferAddStr(&buf, virDomainNetGetModelString(net));
    }

    if (usingVirtio) {
        if (net->driver.virtio.txmode &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_TX_ALG)) {
            virBufferAddLit(&buf, ",tx=");
            switch (net->driver.virtio.txmode) {
                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_IOTHREAD:
                    virBufferAddLit(&buf, "bh");
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_TIMER:
                    virBufferAddLit(&buf, "timer");
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_DEFAULT:
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_LAST:
                default:
                    /* this should never happen, if it does, we need
                     * to add another case to this switch.
                     */
                    virReportEnumRangeError(virDomainNetVirtioTxModeType,
                                            net->driver.virtio.txmode);
                    return NULL;
            }
        }
        qemuBuildIoEventFdStr(&buf, net->driver.virtio.ioeventfd, qemuCaps);
        if (net->driver.virtio.event_idx &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_NET_EVENT_IDX)) {
            virBufferAsprintf(&buf, ",event_idx=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.event_idx));
        }
        if (net->driver.virtio.host.csum) {
            virBufferAsprintf(&buf, ",csum=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.csum));
        }
        if (net->driver.virtio.host.gso) {
            virBufferAsprintf(&buf, ",gso=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.gso));
        }
        if (net->driver.virtio.host.tso4) {
            virBufferAsprintf(&buf, ",host_tso4=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.tso4));
        }
        if (net->driver.virtio.host.tso6) {
            virBufferAsprintf(&buf, ",host_tso6=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.tso6));
        }
        if (net->driver.virtio.host.ecn) {
            virBufferAsprintf(&buf, ",host_ecn=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.ecn));
        }
        if (net->driver.virtio.host.ufo) {
            virBufferAsprintf(&buf, ",host_ufo=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.ufo));
        }
        if (net->driver.virtio.host.mrg_rxbuf) {
            virBufferAsprintf(&buf, ",mrg_rxbuf=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.host.mrg_rxbuf));
        }
        if (net->driver.virtio.guest.csum) {
            virBufferAsprintf(&buf, ",guest_csum=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.csum));
        }
        if (net->driver.virtio.guest.tso4) {
            virBufferAsprintf(&buf, ",guest_tso4=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.tso4));
        }
        if (net->driver.virtio.guest.tso6) {
            virBufferAsprintf(&buf, ",guest_tso6=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.tso6));
        }
        if (net->driver.virtio.guest.ecn) {
            virBufferAsprintf(&buf, ",guest_ecn=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.ecn));
        }
        if (net->driver.virtio.guest.ufo) {
            virBufferAsprintf(&buf, ",guest_ufo=%s",
                              virTristateSwitchTypeToString(net->driver.virtio.guest.ufo));
        }

        if (vhostfdSize > 1) {
            if (net->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
                /* ccw provides a one to one relation of fds to queues and
                 * does not support the vectors option
                 */
                virBufferAddLit(&buf, ",mq=on");
            } else {
                /* As advised at https://www.linux-kvm.org/page/Multiqueue
                 * we should add vectors=2*N+2 where N is the vhostfdSize
                 */
                virBufferAsprintf(&buf, ",mq=on,vectors=%zu", 2 * vhostfdSize + 2);
            }
        }

        if (net->driver.virtio.rx_queue_size)
            virBufferAsprintf(&buf, ",rx_queue_size=%u", net->driver.virtio.rx_queue_size);

        if (net->driver.virtio.tx_queue_size)
            virBufferAsprintf(&buf, ",tx_queue_size=%u", net->driver.virtio.tx_queue_size);

        if (net->mtu)
            virBufferAsprintf(&buf, ",host_mtu=%u", net->mtu);

        if (net->teaming && net->teaming->type == VIR_DOMAIN_NET_TEAMING_TYPE_PERSISTENT)
            virBufferAddLit(&buf, ",failover=on");
    }

    virBufferAsprintf(&buf, ",netdev=host%s", net->info.alias);
    virBufferAsprintf(&buf, ",id=%s", net->info.alias);
    virBufferAsprintf(&buf, ",mac=%s",
                      virMacAddrFormat(&net->mac, macaddr));

    if (qemuBuildDeviceAddressStr(&buf, def, &net->info) < 0)
        return NULL;
    if (qemuBuildRomStr(&buf, &net->info) < 0)
        return NULL;
    if (bootindex)
        virBufferAsprintf(&buf, ",bootindex=%u", bootindex);
    if (usingVirtio)
        qemuBuildVirtioOptionsStr(&buf, net->virtio);

    return virBufferContentAndReset(&buf);
}


virJSONValue *
qemuBuildHostNetStr(virDomainNetDef *net,
                    char **tapfd,
                    size_t tapfdSize,
                    char **vhostfd,
                    size_t vhostfdSize,
                    const char *slirpfd,
                    const char *vdpadev)
{
    bool is_tap = false;
    virDomainNetType netType = virDomainNetGetActualType(net);
    size_t i;

    g_autoptr(virJSONValue) netprops = NULL;

    if (net->script && netType != VIR_DOMAIN_NET_TYPE_ETHERNET) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("scripts are not supported on interfaces of type %s"),
                       virDomainNetTypeToString(netType));
        return NULL;
    }

    switch (netType) {
        /*
         * If type='bridge', and we're running as privileged user
         * or -netdev bridge is not supported then it will fall
         * through, -net tap,fd
         */
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (virJSONValueObjectCreate(&netprops, "s:type", "tap", NULL) < 0)
            return NULL;

        /* for one tapfd 'fd=' shall be used,
         * for more than one 'fds=' is the right choice */
        if (tapfdSize == 1) {
            if (virJSONValueObjectAdd(netprops, "s:fd", tapfd[0], NULL) < 0)
                return NULL;
        } else {
            g_auto(virBuffer) fdsbuf = VIR_BUFFER_INITIALIZER;

            for (i = 0; i < tapfdSize; i++)
                virBufferAsprintf(&fdsbuf, "%s:", tapfd[i]);

            virBufferTrim(&fdsbuf, ":");

            if (virJSONValueObjectAdd(netprops,
                                      "s:fds", virBufferCurrentContent(&fdsbuf),
                                      NULL) < 0)
                return NULL;
        }

        is_tap = true;
        break;

    case VIR_DOMAIN_NET_TYPE_CLIENT:
        if (virJSONValueObjectCreate(&netprops, "s:type", "socket", NULL) < 0 ||
            virJSONValueObjectAppendStringPrintf(netprops, "connect", "%s:%d",
                                                 net->data.socket.address,
                                                 net->data.socket.port) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
        if (virJSONValueObjectCreate(&netprops, "s:type", "socket", NULL) < 0 ||
            virJSONValueObjectAppendStringPrintf(netprops, "listen", "%s:%d",
                                                 NULLSTR_EMPTY(net->data.socket.address),
                                                 net->data.socket.port) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_MCAST:
        if (virJSONValueObjectCreate(&netprops, "s:type", "socket", NULL) < 0 ||
            virJSONValueObjectAppendStringPrintf(netprops, "mcast", "%s:%d",
                                                 net->data.socket.address,
                                                 net->data.socket.port) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_UDP:
        if (virJSONValueObjectCreate(&netprops, "s:type", "socket", NULL) < 0 ||
            virJSONValueObjectAppendStringPrintf(netprops, "udp", "%s:%d",
                                                 net->data.socket.address,
                                                 net->data.socket.port) < 0 ||
            virJSONValueObjectAppendStringPrintf(netprops, "localaddr", "%s:%d",
                                                 net->data.socket.localaddr,
                                                 net->data.socket.localport) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
        if (slirpfd) {
            if (virJSONValueObjectCreate(&netprops, "s:type", "socket", NULL) < 0 ||
                virJSONValueObjectAppendString(netprops, "fd", slirpfd) < 0)
                return NULL;
        } else {
            if (virJSONValueObjectCreate(&netprops, "s:type", "user", NULL) < 0)
                return NULL;

            for (i = 0; i < net->guestIP.nips; i++) {
                const virNetDevIPAddr *ip = net->guestIP.ips[i];
                g_autofree char *addr = NULL;

                if (!(addr = virSocketAddrFormat(&ip->address)))
                    return NULL;

                if (VIR_SOCKET_ADDR_IS_FAMILY(&ip->address, AF_INET)) {
                    g_autofree char *ipv4netaddr = NULL;

                    if (ip->prefix)
                        ipv4netaddr = g_strdup_printf("%s/%u", addr, ip->prefix);
                    else
                        ipv4netaddr = g_strdup(addr);

                    if (virJSONValueObjectAppendString(netprops, "net", ipv4netaddr) < 0)
                        return NULL;
                } else if (VIR_SOCKET_ADDR_IS_FAMILY(&ip->address, AF_INET6)) {
                    if (virJSONValueObjectAppendString(netprops, "ipv6-prefix", addr) < 0)
                        return NULL;
                    if (ip->prefix &&
                        virJSONValueObjectAppendNumberUlong(netprops, "ipv6-prefixlen",
                                                            ip->prefix) < 0)
                        return NULL;
                }
            }
        }
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        if (virJSONValueObjectCreate(&netprops, "s:type", "user", NULL) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        if (virJSONValueObjectCreate(&netprops, "s:type", "vhost-user", NULL) < 0 ||
            virJSONValueObjectAppendStringPrintf(netprops, "chardev", "char%s", net->info.alias) < 0)
            return NULL;

        if (net->driver.virtio.queues > 1 &&
            virJSONValueObjectAppendNumberUlong(netprops, "queues", net->driver.virtio.queues) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_VDPA:
        /* Caller will pass the fd to qemu with add-fd */
        if (virJSONValueObjectCreate(&netprops, "s:type", "vhost-vdpa", NULL) < 0 ||
            virJSONValueObjectAppendString(netprops, "vhostdev", vdpadev) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        /* Should have been handled earlier via PCI/USB hotplug code. */
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    if (virJSONValueObjectAppendStringPrintf(netprops, "id", "host%s", net->info.alias) < 0)
        return NULL;

    if (is_tap) {
        if (vhostfdSize) {
            if (virJSONValueObjectAppendBoolean(netprops, "vhost", true) < 0)
                return NULL;

            if (vhostfdSize == 1) {
                if (virJSONValueObjectAdd(netprops, "s:vhostfd", vhostfd[0], NULL) < 0)
                    return NULL;
            } else {
                g_auto(virBuffer) fdsbuf = VIR_BUFFER_INITIALIZER;

                for (i = 0; i < vhostfdSize; i++)
                    virBufferAsprintf(&fdsbuf, "%s:", vhostfd[i]);

                virBufferTrim(&fdsbuf, ":");

                if (virJSONValueObjectAdd(netprops,
                                          "s:vhostfds", virBufferCurrentContent(&fdsbuf),
                                          NULL) < 0)
                    return NULL;
            }
        }

        if (net->tune.sndbuf_specified &&
            virJSONValueObjectAppendNumberUlong(netprops, "sndbuf", net->tune.sndbuf) < 0)
            return NULL;
    }

    return g_steal_pointer(&netprops);
}


char *
qemuBuildWatchdogDevStr(const virDomainDef *def,
                        virDomainWatchdogDef *dev,
                        virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    const char *model = virDomainWatchdogModelTypeToString(dev->model);
    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing watchdog model"));
        return NULL;
    }

    virBufferAsprintf(&buf, "%s,id=%s", model, dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildWatchdogCommandLine(virCommand *cmd,
                             const virDomainDef *def,
                             virQEMUCaps *qemuCaps)
{
    virDomainWatchdogDef *watchdog = def->watchdog;
    g_autofree char *optstr = NULL;
    const char *action;
    int actualAction;

    if (!def->watchdog)
        return 0;

    if (qemuCommandAddExtDevice(cmd, &def->watchdog->info) < 0)
        return -1;

    virCommandAddArg(cmd, "-device");

    optstr = qemuBuildWatchdogDevStr(def, watchdog, qemuCaps);
    if (!optstr)
        return -1;

    virCommandAddArg(cmd, optstr);

    /* qemu doesn't have a 'dump' action; we tell qemu to 'pause', then
       libvirt listens for the watchdog event, and we perform the dump
       ourselves. so convert 'dump' to 'pause' for the qemu cli */
    actualAction = watchdog->action;
    if (watchdog->action == VIR_DOMAIN_WATCHDOG_ACTION_DUMP)
        actualAction = VIR_DOMAIN_WATCHDOG_ACTION_PAUSE;

    action = virDomainWatchdogActionTypeToString(actualAction);
    if (!action) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("invalid watchdog action"));
        return -1;
    }
    virCommandAddArgList(cmd, "-watchdog-action", action, NULL);

    return 0;
}


static int
qemuBuildMemballoonCommandLine(virCommand *cmd,
                               const virDomainDef *def,
                               virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!virDomainDefHasMemballoon(def))
        return 0;

    if (qemuBuildVirtioDevStr(&buf, "virtio-balloon", qemuCaps,
                              VIR_DOMAIN_DEVICE_MEMBALLOON,
                              def->memballoon) < 0) {
        return -1;
    }

    virBufferAsprintf(&buf, ",id=%s", def->memballoon->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &def->memballoon->info) < 0)
        return -1;

    if (def->memballoon->autodeflate != VIR_TRISTATE_SWITCH_ABSENT) {
        virBufferAsprintf(&buf, ",deflate-on-oom=%s",
                          virTristateSwitchTypeToString(def->memballoon->autodeflate));
    }

    if (def->memballoon->free_page_reporting != VIR_TRISTATE_SWITCH_ABSENT) {
        virBufferAsprintf(&buf, ",free-page-reporting=%s",
                          virTristateSwitchTypeToString(def->memballoon->free_page_reporting));
    }

    qemuBuildVirtioOptionsStr(&buf, def->memballoon->virtio);

    if (qemuCommandAddExtDevice(cmd, &def->memballoon->info) < 0)
        return -1;

    virCommandAddArg(cmd, "-device");
    virCommandAddArgBuffer(cmd, &buf);
    return 0;
}


static char *
qemuBuildNVRAMDevStr(virDomainNVRAMDef *dev)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&buf, "spapr-nvram.reg=0x%llx",
                      dev->info.addr.spaprvio.reg);

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildNVRAMCommandLine(virCommand *cmd,
                          const virDomainDef *def)
{
    g_autofree char *optstr = NULL;

    if (!def->nvram)
        return 0;

    virCommandAddArg(cmd, "-global");
    optstr = qemuBuildNVRAMDevStr(def->nvram);
    if (!optstr)
        return -1;

    virCommandAddArg(cmd, optstr);

    return 0;
}


static char *
qemuBuildVirtioInputDevStr(const virDomainDef *def,
                           virDomainInputDef *dev,
                           virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    switch ((virDomainInputType)dev->type) {
    case VIR_DOMAIN_INPUT_TYPE_MOUSE:
        if (qemuBuildVirtioDevStr(&buf, "virtio-mouse", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            return NULL;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_TABLET:
        if (qemuBuildVirtioDevStr(&buf, "virtio-tablet", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            return NULL;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_KBD:
        if (qemuBuildVirtioDevStr(&buf, "virtio-keyboard", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            return NULL;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_PASSTHROUGH:
        if (qemuBuildVirtioDevStr(&buf, "virtio-input-host", qemuCaps,
                                  VIR_DOMAIN_DEVICE_INPUT, dev) < 0) {
            return NULL;
        }
        break;
    case VIR_DOMAIN_INPUT_TYPE_EVDEV:
    case VIR_DOMAIN_INPUT_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainInputType, dev->type);
        return NULL;
    }

    virBufferAsprintf(&buf, ",id=%s", dev->info.alias);

    if (dev->type == VIR_DOMAIN_INPUT_TYPE_PASSTHROUGH) {
        virBufferAddLit(&buf, ",evdev=");
        virQEMUBuildBufferEscapeComma(&buf, dev->source.evdev);
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info) < 0)
        return NULL;

    qemuBuildVirtioOptionsStr(&buf, dev->virtio);

    return virBufferContentAndReset(&buf);
}

static char *
qemuBuildUSBInputDevStr(const virDomainDef *def,
                        virDomainInputDef *dev,
                        virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    switch (dev->type) {
    case VIR_DOMAIN_INPUT_TYPE_MOUSE:
        virBufferAsprintf(&buf, "usb-mouse,id=%s", dev->info.alias);
        break;
    case VIR_DOMAIN_INPUT_TYPE_TABLET:
        virBufferAsprintf(&buf, "usb-tablet,id=%s", dev->info.alias);
        break;
    case VIR_DOMAIN_INPUT_TYPE_KBD:
        virBufferAsprintf(&buf, "usb-kbd,id=%s", dev->info.alias);
        break;
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildObjectInputDevStr(virDomainInputDef *dev,
                           virQEMUCaps *qemuCaps)
{
    g_autoptr(virJSONValue) props = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (qemuMonitorCreateObjectProps(&props, "input-linux", dev->info.alias,
                                     "s:evdev", dev->source.evdev,
                                     "T:repeat", dev->source.repeat,
                                     NULL) < 0)
        return NULL;

    if (dev->source.grab == VIR_DOMAIN_INPUT_SOURCE_GRAB_ALL)
        virJSONValueObjectAdd(props, "b:grab_all", true, NULL);

    if (dev->source.grabToggle != VIR_DOMAIN_INPUT_SOURCE_GRAB_TOGGLE_DEFAULT)
        virJSONValueObjectAdd(props, "s:grab-toggle",
                              virDomainInputSourceGrabToggleTypeToString(dev->source.grabToggle),
                              NULL);

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, qemuCaps) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


int
qemuBuildInputDevStr(char **devstr,
                     const virDomainDef *def,
                     virDomainInputDef *input,
                     virQEMUCaps *qemuCaps)
{
    switch (input->bus) {
    case VIR_DOMAIN_INPUT_BUS_USB:
        if (!(*devstr = qemuBuildUSBInputDevStr(def, input, qemuCaps)))
            return -1;
        break;

    case VIR_DOMAIN_INPUT_BUS_VIRTIO:
        if (!(*devstr = qemuBuildVirtioInputDevStr(def, input, qemuCaps)))
            return -1;
        break;
    case VIR_DOMAIN_INPUT_BUS_NONE:
        if (!(*devstr = qemuBuildObjectInputDevStr(input, qemuCaps)))
            return -1;
        break;
    }
    return 0;
}


static int
qemuBuildInputCommandLine(virCommand *cmd,
                          const virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ninputs; i++) {
        virDomainInputDef *input = def->inputs[i];
        g_autofree char *devstr = NULL;

        if (qemuCommandAddExtDevice(cmd, &input->info) < 0)
            return -1;

        if (qemuBuildInputDevStr(&devstr, def, input, qemuCaps) < 0)
            return -1;

        if (devstr) {
            if (input->type == VIR_DOMAIN_INPUT_TYPE_EVDEV)
                virCommandAddArg(cmd, "-object");
            else
                virCommandAddArg(cmd, "-device");
            virCommandAddArg(cmd, devstr);
        }
    }

    return 0;
}

static char *
qemuGetAudioIDString(const virDomainDef *def, int id)
{
    virDomainAudioDef *audio = virDomainDefFindAudioByID(def, id);
    if (!audio) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to find audio backend for sound device"));
        return NULL;
    }
    return g_strdup_printf("audio%d", audio->id);
}

static char *
qemuBuildSoundDevStr(const virDomainDef *def,
                     virDomainSoundDef *sound,
                     virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *model = NULL;

    /* Hack for devices with different names in QEMU and libvirt */
    switch (sound->model) {
    case VIR_DOMAIN_SOUND_MODEL_ES1370:
        model = "ES1370";
        break;
    case VIR_DOMAIN_SOUND_MODEL_AC97:
        model = "AC97";
        break;
    case VIR_DOMAIN_SOUND_MODEL_ICH6:
        model = "intel-hda";
        break;
    case VIR_DOMAIN_SOUND_MODEL_USB:
        model = "usb-audio";
        break;
    case VIR_DOMAIN_SOUND_MODEL_ICH9:
        model = "ich9-intel-hda";
        break;
    case VIR_DOMAIN_SOUND_MODEL_SB16:
        model = "sb16";
        break;
    case VIR_DOMAIN_SOUND_MODEL_PCSPK: /* pc-speaker is handled separately */
    case VIR_DOMAIN_SOUND_MODEL_ICH7:
    case VIR_DOMAIN_SOUND_MODEL_LAST:
        return NULL;
    }

    virBufferAsprintf(&buf, "%s,id=%s", model, sound->info.alias);
    if (!virDomainSoundModelSupportsCodecs(sound) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_AUDIODEV)) {
        g_autofree char *audioid = qemuGetAudioIDString(def, sound->audioId);
        if (!audioid)
            return NULL;
        virBufferAsprintf(&buf, ",audiodev=%s", audioid);
    }
    if (qemuBuildDeviceAddressStr(&buf, def, &sound->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSoundCodecStr(const virDomainDef *def,
                       virDomainSoundDef *sound,
                       virDomainSoundCodecDef *codec,
                       virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *stype;
    int type;

    type = codec->type;
    stype = qemuSoundCodecTypeToString(type);

    virBufferAsprintf(&buf, "%s,id=%s-codec%d,bus=%s.0,cad=%d",
                      stype, sound->info.alias, codec->cad, sound->info.alias, codec->cad);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_AUDIODEV)) {
        g_autofree char *audioid = qemuGetAudioIDString(def, sound->audioId);
        if (!audioid)
            return NULL;
        virBufferAsprintf(&buf, ",audiodev=%s", audioid);
    }

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildSoundCommandLine(virCommand *cmd,
                          const virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    size_t i, j;

    for (i = 0; i < def->nsounds; i++) {
        virDomainSoundDef *sound = def->sounds[i];
        g_autofree char *str = NULL;

        /* Sadly pcspk device doesn't use -device syntax. Fortunately
         * we don't need to set any PCI address on it, so we don't
         * mind too much */
        if (sound->model == VIR_DOMAIN_SOUND_MODEL_PCSPK) {
            virCommandAddArgList(cmd, "-soundhw", "pcspk", NULL);
        } else {
            if (qemuCommandAddExtDevice(cmd, &sound->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");
            if (!(str = qemuBuildSoundDevStr(def, sound, qemuCaps)))
                return -1;

            virCommandAddArg(cmd, str);
            if (virDomainSoundModelSupportsCodecs(sound)) {
                for (j = 0; j < sound->ncodecs; j++) {
                    g_autofree char *codecstr = NULL;
                    virCommandAddArg(cmd, "-device");
                    if (!(codecstr =
                          qemuBuildSoundCodecStr(def, sound,
                                                 sound->codecs[j], qemuCaps))) {
                        return -1;

                    }
                    virCommandAddArg(cmd, codecstr);
                }
                if (j == 0) {
                    g_autofree char *codecstr = NULL;
                    virDomainSoundCodecDef codec = {
                        VIR_DOMAIN_SOUND_CODEC_TYPE_DUPLEX,
                        0
                    };
                    virCommandAddArg(cmd, "-device");
                    if (!(codecstr =
                          qemuBuildSoundCodecStr(def, sound,
                                                 &codec, qemuCaps))) {
                        return -1;

                    }
                    virCommandAddArg(cmd, codecstr);
                }
            }
        }
    }
    return 0;
}


static const char *
qemuDeviceVideoGetModel(virQEMUCaps *qemuCaps,
                        const virDomainVideoDef *video,
                        bool *virtio)
{
    const char *model = NULL;
    bool primaryVga = false;
    virTristateSwitch accel3d = VIR_TRISTATE_SWITCH_ABSENT;

    *virtio = false;

    if (video->accel)
        accel3d = video->accel->accel3d;

    if (video->primary && qemuDomainSupportsVideoVga(video, qemuCaps))
        primaryVga = true;

    /* We try to chose the best model for primary video device by preferring
     * model with VGA compatibility mode.  For some video devices on some
     * architectures there might not be such model so fallback to one
     * without VGA compatibility mode. */
    if (video->backend == VIR_DOMAIN_VIDEO_BACKEND_TYPE_VHOSTUSER) {
        if (primaryVga) {
            model = "vhost-user-vga";
        } else {
            model = "vhost-user-gpu";
            *virtio = true;
        }
    } else {
        if (primaryVga) {
            switch ((virDomainVideoType) video->type) {
            case VIR_DOMAIN_VIDEO_TYPE_VGA:
                model = "VGA";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
                model = "cirrus-vga";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
                model = "vmware-svga";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_QXL:
                model = "qxl-vga";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_VIRTIO:
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_VGA_GL) &&
                    accel3d == VIR_TRISTATE_SWITCH_ON)
                    model = "virtio-vga-gl";
                else
                    model = "virtio-vga";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_BOCHS:
                model = "bochs-display";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_RAMFB:
                model = "ramfb";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_DEFAULT:
            case VIR_DOMAIN_VIDEO_TYPE_XEN:
            case VIR_DOMAIN_VIDEO_TYPE_VBOX:
            case VIR_DOMAIN_VIDEO_TYPE_PARALLELS:
            case VIR_DOMAIN_VIDEO_TYPE_GOP:
            case VIR_DOMAIN_VIDEO_TYPE_NONE:
            case VIR_DOMAIN_VIDEO_TYPE_LAST:
                break;
            }
        } else {
            switch ((virDomainVideoType) video->type) {
            case VIR_DOMAIN_VIDEO_TYPE_QXL:
                model = "qxl";
                break;
            case VIR_DOMAIN_VIDEO_TYPE_VIRTIO:
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_GPU_GL_PCI) &&
                    accel3d == VIR_TRISTATE_SWITCH_ON)
                    model = "virtio-gpu-gl";
                else
                    model = "virtio-gpu";
                *virtio = true;
                break;
            case VIR_DOMAIN_VIDEO_TYPE_DEFAULT:
            case VIR_DOMAIN_VIDEO_TYPE_VGA:
            case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
            case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
            case VIR_DOMAIN_VIDEO_TYPE_XEN:
            case VIR_DOMAIN_VIDEO_TYPE_VBOX:
            case VIR_DOMAIN_VIDEO_TYPE_PARALLELS:
            case VIR_DOMAIN_VIDEO_TYPE_GOP:
            case VIR_DOMAIN_VIDEO_TYPE_NONE:
            case VIR_DOMAIN_VIDEO_TYPE_BOCHS:
            case VIR_DOMAIN_VIDEO_TYPE_RAMFB:
            case VIR_DOMAIN_VIDEO_TYPE_LAST:
                break;
            }
        }
    }

    if (!model || STREQ(model, "")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid model for video type '%s'"),
                       virDomainVideoTypeToString(video->type));
        return NULL;
    }

    return model;
}


static char *
qemuBuildDeviceVideoStr(const virDomainDef *def,
                        virDomainVideoDef *video,
                        virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *model = NULL;
    virTristateSwitch accel3d = VIR_TRISTATE_SWITCH_ABSENT;
    bool virtio = false;

    if (video->accel)
        accel3d = video->accel->accel3d;

    if (!(model = qemuDeviceVideoGetModel(qemuCaps, video, &virtio)))
        return NULL;

    if (virtio) {
        if (qemuBuildVirtioDevStr(&buf, model, qemuCaps,
                                  VIR_DOMAIN_DEVICE_VIDEO, video) < 0) {
            return NULL;
        }
    } else {
        virBufferAsprintf(&buf, "%s", model);
    }

    virBufferAsprintf(&buf, ",id=%s", video->info.alias);

    if (video->backend != VIR_DOMAIN_VIDEO_BACKEND_TYPE_VHOSTUSER &&
        video->type == VIR_DOMAIN_VIDEO_TYPE_VIRTIO) {
        if (video->accel &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_GPU_VIRGL) &&
            (accel3d == VIR_TRISTATE_SWITCH_ON ||
             accel3d == VIR_TRISTATE_SWITCH_OFF)) {
            virBufferAsprintf(&buf, ",virgl=%s",
                              virTristateSwitchTypeToString(accel3d));
        }
    }

    if (video->type == VIR_DOMAIN_VIDEO_TYPE_QXL) {
        if (video->ram) {
            /* QEMU accepts bytes for ram_size. */
            virBufferAsprintf(&buf, ",ram_size=%u", video->ram * 1024);
        }

        if (video->vram) {
            /* QEMU accepts bytes for vram_size. */
            virBufferAsprintf(&buf, ",vram_size=%u", video->vram * 1024);
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_VRAM64)) {
            /* QEMU accepts mebibytes for vram64_size_mb. */
            virBufferAsprintf(&buf, ",vram64_size_mb=%u", video->vram64 / 1024);
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_VGAMEM)) {
            /* QEMU accepts mebibytes for vgamem_mb. */
            virBufferAsprintf(&buf, ",vgamem_mb=%u", video->vgamem / 1024);
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QXL_MAX_OUTPUTS)) {
            if (video->heads)
                virBufferAsprintf(&buf, ",max_outputs=%u", video->heads);
        }
    } else if (video->backend == VIR_DOMAIN_VIDEO_BACKEND_TYPE_VHOSTUSER) {
        g_autofree char *alias = qemuDomainGetVhostUserChrAlias(video->info.alias);
        if (video->heads)
            virBufferAsprintf(&buf, ",max_outputs=%u", video->heads);
        virBufferAsprintf(&buf, ",chardev=%s", alias);
    } else if (video->type == VIR_DOMAIN_VIDEO_TYPE_VIRTIO) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_GPU_MAX_OUTPUTS)) {
            if (video->heads)
                virBufferAsprintf(&buf, ",max_outputs=%u", video->heads);
        }
    } else if ((video->type == VIR_DOMAIN_VIDEO_TYPE_VGA &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA_VGAMEM)) ||
               (video->type == VIR_DOMAIN_VIDEO_TYPE_VMVGA &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_VMWARE_SVGA_VGAMEM))) {
        if (video->vram)
            virBufferAsprintf(&buf, ",vgamem_mb=%u", video->vram / 1024);
    } else if (video->type == VIR_DOMAIN_VIDEO_TYPE_BOCHS) {
        if (video->vram)
            virBufferAsprintf(&buf, ",vgamem=%uk", video->vram);
    }

    if (video->res && video->res->x && video->res->y) {
        /* QEMU accepts resolution xres and yres. */
        virBufferAsprintf(&buf, ",xres=%u,yres=%u", video->res->x, video->res->y);
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &video->info) < 0)
        return NULL;

    qemuBuildVirtioOptionsStr(&buf, video->virtio);

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildVhostUserChardevStr(const char *alias,
                             int *fd,
                             virCommand *cmd)
{
    g_autofree char *chardev_alias = qemuDomainGetVhostUserChrAlias(alias);
    char *chardev = NULL;

    if (*fd == -1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Attempt to pass closed vhostuser FD"));
        return NULL;
    }

    chardev = g_strdup_printf("socket,id=%s,fd=%d", chardev_alias, *fd);

    virCommandPassFD(cmd, *fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
    *fd = -1;

    return chardev;
}


static int
qemuBuildVideoCommandLine(virCommand *cmd,
                          const virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    size_t i;

    for (i = 0; i < def->nvideos; i++) {
        g_autofree char *chardev = NULL;
        virDomainVideoDef *video = def->videos[i];

        if (video->backend == VIR_DOMAIN_VIDEO_BACKEND_TYPE_VHOSTUSER) {
            if (!(chardev = qemuBuildVhostUserChardevStr(video->info.alias,
                                &QEMU_DOMAIN_VIDEO_PRIVATE(video)->vhost_user_fd,
                                cmd)))
                return -1;

            virCommandAddArgList(cmd, "-chardev", chardev, NULL);
        }
    }

    for (i = 0; i < def->nvideos; i++) {
        g_autofree char *str = NULL;
        virDomainVideoDef *video = def->videos[i];

        if (video->type == VIR_DOMAIN_VIDEO_TYPE_NONE)
            continue;

        if (qemuCommandAddExtDevice(cmd, &def->videos[i]->info) < 0)
            return -1;

        virCommandAddArg(cmd, "-device");

        if (!(str = qemuBuildDeviceVideoStr(def, video, qemuCaps)))
            return -1;

        virCommandAddArg(cmd, str);
    }

    return 0;
}


char *
qemuBuildPCIHostdevDevStr(const virDomainDef *def,
                          virDomainHostdevDef *dev,
                          unsigned int bootIndex, /* used iff dev->info->bootIndex == 0 */
                          virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysPCI *pcisrc = &dev->source.subsys.u.pci;
    int backend = pcisrc->backend;
    virDomainNetTeamingInfo *teaming;

    /* caller has to assign proper passthrough backend type */
    switch ((virDomainHostdevSubsysPCIBackendType)backend) {
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO:
        virBufferAddLit(&buf, "vfio-pci");
        break;

    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_KVM:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_XEN:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid PCI passthrough type '%s'"),
                       virDomainHostdevSubsysPCIBackendTypeToString(backend));
        return NULL;
    }

    virBufferAddLit(&buf, ",host=");
    virBufferAsprintf(&buf,
                      VIR_PCI_DEVICE_ADDRESS_FMT,
                      pcisrc->addr.domain,
                      pcisrc->addr.bus,
                      pcisrc->addr.slot,
                      pcisrc->addr.function);
    virBufferAsprintf(&buf, ",id=%s", dev->info->alias);
    if (dev->info->bootIndex)
        bootIndex = dev->info->bootIndex;
    if (bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", bootIndex);
    if (qemuBuildDeviceAddressStr(&buf, def, dev->info) < 0)
        return NULL;
    if (qemuBuildRomStr(&buf, dev->info) < 0)
        return NULL;

    if (dev->parentnet)
        teaming = dev->parentnet->teaming;
    else
        teaming = dev->teaming;

    if (teaming &&
        teaming->type == VIR_DOMAIN_NET_TEAMING_TYPE_TRANSIENT &&
        teaming->persistent) {
        virBufferAsprintf(&buf,  ",failover_pair_id=%s", teaming->persistent);
    }

    return virBufferContentAndReset(&buf);
}


char *
qemuBuildUSBHostdevDevStr(const virDomainDef *def,
                          virDomainHostdevDef *dev,
                          virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysUSB *usbsrc = &dev->source.subsys.u.usb;

    virBufferAddLit(&buf, "usb-host");
    if (!dev->missing) {
        if (usbsrc->bus == 0 && usbsrc->device == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("USB host device is missing bus/device information"));
            return NULL;
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_HOST_HOSTDEVICE)) {
            virBufferAsprintf(&buf, ",hostdevice=/dev/bus/usb/%03d/%03d",
                              usbsrc->bus, usbsrc->device);
        } else {
            virBufferAsprintf(&buf, ",hostbus=%d,hostaddr=%d",
                              usbsrc->bus, usbsrc->device);
        }
    }
    virBufferAsprintf(&buf, ",id=%s", dev->info->alias);
    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info->bootIndex);

    if (qemuBuildDeviceAddressStr(&buf, def, dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildHubDevStr(const virDomainDef *def,
                   virDomainHubDef *dev,
                   virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "usb-hub");
    virBufferAsprintf(&buf, ",id=%s", dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildHubCommandLine(virCommand *cmd,
                        const virDomainDef *def,
                        virQEMUCaps *qemuCaps)
{
    size_t i;

    for (i = 0; i < def->nhubs; i++) {
        virDomainHubDef *hub = def->hubs[i];
        g_autofree char *optstr = NULL;

        virCommandAddArg(cmd, "-device");
        if (!(optstr = qemuBuildHubDevStr(def, hub, qemuCaps)))
            return -1;
        virCommandAddArg(cmd, optstr);
    }

    return 0;
}


static char *
qemuBuildSCSIiSCSIHostdevDrvStr(virDomainHostdevDef *dev,
                                virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *netsource = NULL;
    g_autoptr(virJSONValue) srcprops = NULL;
    virDomainHostdevSubsysSCSI *scsisrc = &dev->source.subsys.u.scsi;
    virDomainHostdevSubsysSCSIiSCSI *iscsisrc = &scsisrc->u.iscsi;
    qemuDomainStorageSourcePrivate *srcPriv =
        QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(iscsisrc->src);

    if (qemuDiskSourceNeedsProps(iscsisrc->src, qemuCaps)) {
        if (!(srcprops = qemuDiskSourceGetProps(iscsisrc->src)))
            return NULL;
        if (!(netsource = virQEMUBuildDriveCommandlineFromJSON(srcprops)))
            return NULL;
        virBufferAsprintf(&buf, "%s,if=none,format=raw", netsource);
    } else {
        /* Rather than pull what we think we want - use the network disk code */
        if (!(netsource = qemuBuildNetworkDriveStr(iscsisrc->src, srcPriv ?
                                                   srcPriv->secinfo : NULL)))
            return NULL;
        virBufferAddLit(&buf, "file=");
        virQEMUBuildBufferEscapeComma(&buf, netsource);
        virBufferAddLit(&buf, ",if=none,format=raw");
    }

    return virBufferContentAndReset(&buf);
}

char *
qemuBuildSCSIVHostHostdevDevStr(const virDomainDef *def,
                           virDomainHostdevDef *dev,
                           virQEMUCaps *qemuCaps,
                           char *vhostfdName)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysSCSIVHost *hostsrc = &dev->source.subsys.u.scsi_host;

    if (qemuBuildVirtioDevStr(&buf, "vhost-scsi", qemuCaps,
                              VIR_DOMAIN_DEVICE_HOSTDEV, dev) < 0) {
        return NULL;
    }

    virBufferAsprintf(&buf, ",wwpn=%s,vhostfd=%s,id=%s",
                      hostsrc->wwpn,
                      vhostfdName,
                      dev->info->alias);

    if (qemuBuildDeviceAddressStr(&buf, def, dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}

static char *
qemuBuildSCSIHostdevDrvStr(virDomainHostdevDef *dev,
                           virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *source = NULL;
    g_autofree char *drivealias = NULL;
    virDomainHostdevSubsysSCSI *scsisrc = &dev->source.subsys.u.scsi;

    if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
        if (!(source = qemuBuildSCSIiSCSIHostdevDrvStr(dev, qemuCaps)))
            return NULL;
        virBufferAdd(&buf, source, -1);
    } else {
        virBufferAsprintf(&buf, "file=%s,if=none,format=raw", scsisrc->u.host.src->path);
    }

    if (!(drivealias = qemuAliasFromHostdev(dev)))
        return NULL;
    virBufferAsprintf(&buf, ",id=%s", drivealias);

    if (dev->readonly)
        virBufferAddLit(&buf, ",readonly=on");

    return virBufferContentAndReset(&buf);
}

char *
qemuBuildSCSIHostdevDevStr(const virDomainDef *def,
                           virDomainHostdevDef *dev,
                           const char *backendAlias)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    int model = -1;
    const char *contAlias;

    model = qemuDomainFindSCSIControllerModel(def, dev->info);
    if (model < 0)
        return NULL;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
        if (dev->info->addr.drive.target != 0) {
           virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for scsi host device "
                             "if its controller model is 'lsilogic'"));
            return NULL;
        }

        if (dev->info->addr.drive.unit > 7) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("unit must be not more than 7 for scsi host "
                             "device if its controller model is 'lsilogic'"));
            return NULL;
        }
    }

    virBufferAddLit(&buf, "scsi-generic");

    if (!(contAlias = virDomainControllerAliasFind(def, VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
                                                   dev->info->addr.drive.controller)))
        return NULL;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
        virBufferAsprintf(&buf, ",bus=%s.%d,scsi-id=%d",
                          contAlias,
                          dev->info->addr.drive.bus,
                          dev->info->addr.drive.unit);
    } else {
        virBufferAsprintf(&buf, ",bus=%s.0,channel=%d,scsi-id=%d,lun=%d",
                          contAlias,
                          dev->info->addr.drive.bus,
                          dev->info->addr.drive.target,
                          dev->info->addr.drive.unit);
    }

    virBufferAsprintf(&buf, ",drive=%s,id=%s", backendAlias, dev->info->alias);

    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info->bootIndex);

    return virBufferContentAndReset(&buf);
}

static int
qemuBuildChrChardevFileStr(virLogManager *logManager,
                           virSecurityManager *secManager,
                           virQEMUDriverConfig *cfg,
                           virQEMUCaps *qemuCaps,
                           const virDomainDef *def,
                           virCommand *cmd,
                           virBuffer *buf,
                           const char *filearg, const char *fileval,
                           const char *appendarg, int appendval)
{
    /* Technically, to pass an FD via /dev/fdset we don't need
     * any capability check because X_QEMU_CAPS_ADD_FD is already
     * assumed. But keeping the old style is still handy when
     * building a standalone command line (e.g. for tests). */
    if (logManager ||
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_FD_PASS_COMMANDLINE)) {
        g_autofree char *fdset = NULL;
        int logfd;
        size_t idx;

        if (logManager) {
            int flags = 0;

            if (appendval == VIR_TRISTATE_SWITCH_ABSENT ||
                appendval == VIR_TRISTATE_SWITCH_OFF)
                flags |= VIR_LOG_MANAGER_PROTOCOL_DOMAIN_OPEN_LOG_FILE_TRUNCATE;

            if ((logfd = virLogManagerDomainOpenLogFile(logManager,
                                                        "qemu",
                                                        def->uuid,
                                                        def->name,
                                                        fileval,
                                                        flags,
                                                        NULL, NULL)) < 0)
                return -1;
        } else {
            int oflags = O_CREAT | O_WRONLY;

            switch (appendval) {
            case VIR_TRISTATE_SWITCH_ABSENT:
            case VIR_TRISTATE_SWITCH_OFF:
                oflags |= O_TRUNC;
                break;
            case VIR_TRISTATE_SWITCH_ON:
                oflags |= O_APPEND;
                break;
            case VIR_TRISTATE_SWITCH_LAST:
                break;
            }

            if ((logfd = qemuDomainOpenFile(cfg, def, fileval, oflags, NULL)) < 0)
                return -1;

            if (qemuSecuritySetImageFDLabel(secManager, (virDomainDef*)def, logfd) < 0) {
                VIR_FORCE_CLOSE(logfd);
                return -1;
            }
        }

        virCommandPassFDIndex(cmd, logfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT, &idx);
        fdset = qemuBuildFDSet(logfd, idx);

        virCommandAddArg(cmd, "-add-fd");
        virCommandAddArg(cmd, fdset);

        virBufferAsprintf(buf, ",%s=/dev/fdset/%zu,%s=on", filearg, idx, appendarg);
    } else {
        virBufferAsprintf(buf, ",%s=", filearg);
        virQEMUBuildBufferEscapeComma(buf, fileval);
        if (appendval != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(buf, ",%s=%s", appendarg,
                              virTristateSwitchTypeToString(appendval));
        }
    }

    return 0;
}


static void
qemuBuildChrChardevReconnectStr(virBuffer *buf,
                                const virDomainChrSourceReconnectDef *def)
{
    if (def->enabled == VIR_TRISTATE_BOOL_YES) {
        virBufferAsprintf(buf, ",reconnect=%u", def->timeout);
    } else if (def->enabled == VIR_TRISTATE_BOOL_NO) {
        virBufferAddLit(buf, ",reconnect=0");
    }
}


int
qemuOpenChrChardevUNIXSocket(const virDomainChrSourceDef *dev)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to create UNIX socket"));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, dev->data.nix.path) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("UNIX socket path '%s' too long"),
                       dev->data.nix.path);
        goto error;
    }

    if (unlink(dev->data.nix.path) < 0 && errno != ENOENT) {
        virReportSystemError(errno,
                             _("Unable to unlink %s"),
                             dev->data.nix.path);
        goto error;
    }

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        virReportSystemError(errno,
                             _("Unable to bind to UNIX socket path '%s'"),
                             dev->data.nix.path);
        goto error;
    }

    if (listen(fd, 1) < 0) {
        virReportSystemError(errno,
                             _("Unable to listen to UNIX socket path '%s'"),
                             dev->data.nix.path);
        goto error;
    }

    /* We run QEMU with umask 0002. Compensate for the umask
     * libvirtd might be running under to get the same permission
     * QEMU would have. */
    if (virFileUpdatePerm(dev->data.nix.path, 0002, 0664) < 0)
        goto error;

    return fd;

 error:
    VIR_FORCE_CLOSE(fd);
    return -1;
}


enum {
    QEMU_BUILD_CHARDEV_TCP_NOWAIT = (1 << 0),
    QEMU_BUILD_CHARDEV_FILE_LOGD  = (1 << 1),
    QEMU_BUILD_CHARDEV_UNIX_FD_PASS = (1 << 2),
};

/* This function outputs a -chardev command line option which describes only the
 * host side of the character device */
static char *
qemuBuildChrChardevStr(virLogManager *logManager,
                       virSecurityManager *secManager,
                       virCommand *cmd,
                       virQEMUDriverConfig *cfg,
                       const virDomainDef *def,
                       const virDomainChrSourceDef *dev,
                       const char *alias,
                       virQEMUCaps *qemuCaps,
                       unsigned int cdevflags)
{
    qemuDomainChrSourcePrivate *chrSourcePriv = QEMU_DOMAIN_CHR_SOURCE_PRIVATE(dev);
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    bool telnet;
    g_autofree char *charAlias = NULL;

    if (!(charAlias = qemuAliasChardevFromDevAlias(alias)))
        return NULL;

    switch (dev->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
        virBufferAsprintf(&buf, "null,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
        virBufferAsprintf(&buf, "vc,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
        virBufferAsprintf(&buf, "pty,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_DEV:
        virBufferAsprintf(&buf, "%s,id=%s,path=",
                          STRPREFIX(alias, "parallel") ? "parport" : "tty",
                          charAlias);
        virQEMUBuildBufferEscapeComma(&buf, dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
        virBufferAsprintf(&buf, "file,id=%s", charAlias);

        if (qemuBuildChrChardevFileStr(cdevflags & QEMU_BUILD_CHARDEV_FILE_LOGD ?
                                       logManager : NULL,
                                       secManager, cfg, qemuCaps, def, cmd, &buf,
                                       "path", dev->data.file.path,
                                       "append", dev->data.file.append) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        virBufferAsprintf(&buf, "pipe,id=%s,path=", charAlias);
        virQEMUBuildBufferEscapeComma(&buf, dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_STDIO:
        virBufferAsprintf(&buf, "stdio,id=%s", charAlias);
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP: {
        const char *connectHost = dev->data.udp.connectHost;
        const char *bindHost = dev->data.udp.bindHost;
        const char *bindService = dev->data.udp.bindService;

        if (connectHost == NULL)
            connectHost = "";
        if (bindHost == NULL)
            bindHost = "";
        if (bindService == NULL)
            bindService = "0";

        virBufferAsprintf(&buf,
                          "udp,id=%s,host=%s,port=%s,localaddr=%s,"
                          "localport=%s",
                          charAlias,
                          connectHost,
                          dev->data.udp.connectService,
                          bindHost, bindService);
        break;
    }
    case VIR_DOMAIN_CHR_TYPE_TCP:
        telnet = dev->data.tcp.protocol == VIR_DOMAIN_CHR_TCP_PROTOCOL_TELNET;
        virBufferAsprintf(&buf,
                          "socket,id=%s,host=%s,port=%s%s",
                          charAlias,
                          dev->data.tcp.host,
                          dev->data.tcp.service,
                          telnet ? ",telnet=on" : "");

        if (dev->data.tcp.listen) {
            virBufferAddLit(&buf, ",server=on");
            if (cdevflags & QEMU_BUILD_CHARDEV_TCP_NOWAIT)
                virBufferAddLit(&buf, ",wait=off");
        }

        qemuBuildChrChardevReconnectStr(&buf, &dev->data.tcp.reconnect);

        if (dev->data.tcp.haveTLS == VIR_TRISTATE_BOOL_YES) {
            g_autofree char *objalias = NULL;
            const char *tlsCertEncSecAlias = NULL;

            /* Add the secret object first if necessary. The
             * secinfo is added only to a TCP serial device during
             * qemuDomainSecretChardevPrepare. Subsequently called
             * functions can just check the config fields */
            if (chrSourcePriv && chrSourcePriv->secinfo) {
                if (qemuBuildObjectSecretCommandLine(cmd,
                                                     chrSourcePriv->secinfo,
                                                     qemuCaps) < 0)
                    return NULL;

                tlsCertEncSecAlias = chrSourcePriv->secinfo->s.aes.alias;
            }

            if (!(objalias = qemuAliasTLSObjFromSrcAlias(charAlias)))
                return NULL;

            if (qemuBuildTLSx509CommandLine(cmd, cfg->chardevTLSx509certdir,
                                            dev->data.tcp.listen,
                                            cfg->chardevTLSx509verify,
                                            tlsCertEncSecAlias,
                                            objalias, qemuCaps) < 0) {
                return NULL;
            }

            virBufferAsprintf(&buf, ",tls-creds=%s", objalias);
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(&buf, "socket,id=%s", charAlias);
        if (dev->data.nix.listen &&
            (cdevflags & QEMU_BUILD_CHARDEV_UNIX_FD_PASS) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_FD_PASS_COMMANDLINE)) {
            int fd;

            if (qemuSecuritySetSocketLabel(secManager, (virDomainDef *)def) < 0)
                return NULL;
            fd = qemuOpenChrChardevUNIXSocket(dev);
            if (qemuSecurityClearSocketLabel(secManager, (virDomainDef *)def) < 0) {
                VIR_FORCE_CLOSE(fd);
                return NULL;
            }
            if (fd < 0)
                return NULL;

            virBufferAsprintf(&buf, ",fd=%d", fd);

            virCommandPassFD(cmd, fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        } else {
            virBufferAddLit(&buf, ",path=");
            virQEMUBuildBufferEscapeComma(&buf, dev->data.nix.path);
        }
        if (dev->data.nix.listen) {
            virBufferAddLit(&buf, ",server=on");
            if (cdevflags & QEMU_BUILD_CHARDEV_TCP_NOWAIT)
                virBufferAddLit(&buf, ",wait=off");
        }

        qemuBuildChrChardevReconnectStr(&buf, &dev->data.nix.reconnect);
        break;

    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        virBufferAsprintf(&buf, "spicevmc,id=%s,name=%s", charAlias,
                          virDomainChrSpicevmcTypeToString(dev->data.spicevmc));
        break;

    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
        virBufferAsprintf(&buf, "spiceport,id=%s,name=%s", charAlias,
                          dev->data.spiceport.channel);
        break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported chardev '%s'"),
                       virDomainChrTypeToString(dev->type));
        return NULL;
    }

    if (dev->logfile) {
        if (qemuBuildChrChardevFileStr(logManager, secManager, cfg,
                                       qemuCaps, def, cmd, &buf,
                                       "logfile", dev->logfile,
                                       "logappend", dev->logappend) < 0)
            return NULL;
    }

    return virBufferContentAndReset(&buf);
}


static const char *
qemuBuildHostdevMdevModelTypeString(virDomainHostdevSubsysMediatedDev *mdev)
{
    /* when the 'ramfb' attribute is set, we must use the nohotplug variant
     * rather than 'vfio-pci' */
    if (mdev->model == VIR_MDEV_MODEL_TYPE_VFIO_PCI &&
        mdev->ramfb == VIR_TRISTATE_SWITCH_ON)
        return "vfio-pci-nohotplug";

    return virMediatedDeviceModelTypeToString(mdev->model);
}


char *
qemuBuildHostdevMediatedDevStr(const virDomainDef *def,
                               virDomainHostdevDef *dev,
                               virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virDomainHostdevSubsysMediatedDev *mdevsrc = &dev->source.subsys.u.mdev;
    g_autofree char *mdevPath = NULL;
    const char *dev_str = NULL;

    mdevPath = virMediatedDeviceGetSysfsPath(mdevsrc->uuidstr);
    dev_str = qemuBuildHostdevMdevModelTypeString(mdevsrc);

    if (!dev_str)
        return NULL;

    virBufferAdd(&buf, dev_str, -1);
    virBufferAsprintf(&buf, ",id=%s,sysfsdev=%s", dev->info->alias, mdevPath);

    if (mdevsrc->display != VIR_TRISTATE_SWITCH_ABSENT)
        virBufferAsprintf(&buf, ",display=%s",
                          virTristateSwitchTypeToString(mdevsrc->display));

    if (qemuBuildDeviceAddressStr(&buf, def, dev->info) < 0)
        return NULL;

    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info->bootIndex);

    if (mdevsrc->ramfb == VIR_TRISTATE_SWITCH_ON)
        virBufferAsprintf(&buf, ",ramfb=%s",
                          virTristateSwitchTypeToString(mdevsrc->ramfb));

    return virBufferContentAndReset(&buf);
}


qemuBlockStorageSourceAttachData *
qemuBuildHostdevSCSIDetachPrepare(virDomainHostdevDef *hostdev,
                                  virQEMUCaps *qemuCaps)
{
    virDomainHostdevSubsysSCSI *scsisrc = &hostdev->source.subsys.u.scsi;
    g_autoptr(qemuBlockStorageSourceAttachData) ret = g_new0(qemuBlockStorageSourceAttachData, 1);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV_HOSTDEV_SCSI)) {
        virStorageSource *src;
        qemuDomainStorageSourcePrivate *srcpriv;

        switch ((virDomainHostdevSCSIProtocolType) scsisrc->protocol) {
        case VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_NONE:
            src = scsisrc->u.host.src;
            break;

        case VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI:
            src = scsisrc->u.iscsi.src;
            break;

        case VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_LAST:
        default:
            virReportEnumRangeError(virDomainHostdevSCSIProtocolType, scsisrc->protocol);
            return NULL;
        }

        srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);
        ret->storageNodeName = src->nodestorage;
        ret->storageAttached = true;

        if (srcpriv && srcpriv->secinfo &&
            srcpriv->secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES)
            ret->authsecretAlias = g_strdup(srcpriv->secinfo->s.aes.alias);

    } else {
        ret->driveAlias = qemuAliasFromHostdev(hostdev);
        ret->driveAdded = true;
    }

    return g_steal_pointer(&ret);
}


qemuBlockStorageSourceAttachData *
qemuBuildHostdevSCSIAttachPrepare(virDomainHostdevDef *hostdev,
                                  const char **backendAlias,
                                  virQEMUCaps *qemuCaps)
{
    virDomainHostdevSubsysSCSI *scsisrc = &hostdev->source.subsys.u.scsi;
    g_autoptr(qemuBlockStorageSourceAttachData) ret = g_new0(qemuBlockStorageSourceAttachData, 1);
    virStorageSource *src = NULL;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV_HOSTDEV_SCSI)) {
        switch ((virDomainHostdevSCSIProtocolType) scsisrc->protocol) {
        case VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_NONE:
            src = scsisrc->u.host.src;
            break;

        case VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI:
            src = scsisrc->u.iscsi.src;
            break;

        case VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_LAST:
        default:
            virReportEnumRangeError(virDomainHostdevSCSIProtocolType, scsisrc->protocol);
            return NULL;
        }

        ret->storageNodeName = src->nodestorage;
        *backendAlias = src->nodestorage;

        if (!(ret->storageProps = qemuBlockStorageSourceGetBackendProps(src,
                                                                        QEMU_BLOCK_STORAGE_SOURCE_BACKEND_PROPS_SKIP_UNMAP)))
            return NULL;

    } else {
        if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI)
            src = scsisrc->u.iscsi.src;
        ret->driveCmd = qemuBuildSCSIHostdevDrvStr(hostdev, qemuCaps);
        ret->driveAlias = qemuAliasFromHostdev(hostdev);
        *backendAlias = ret->driveAlias;
    }

    if (src &&
        qemuBuildStorageSourceAttachPrepareCommon(src, ret, qemuCaps) < 0)
        return NULL;

    return g_steal_pointer(&ret);
}


static int
qemuBuildHostdevSCSICommandLine(virCommand *cmd,
                                const virDomainDef *def,
                                virDomainHostdevDef *hostdev,
                                virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceAttachData) data = NULL;
    g_autofree char *devstr = NULL;
    const char *backendAlias = NULL;

    if (!(data = qemuBuildHostdevSCSIAttachPrepare(hostdev, &backendAlias, qemuCaps)))
        return -1;

    if (qemuBuildBlockStorageSourceAttachDataCommandline(cmd, data, qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-device");
    if (!(devstr = qemuBuildSCSIHostdevDevStr(def, hostdev, backendAlias)))
        return -1;
    virCommandAddArg(cmd, devstr);

    return 0;
}


static int
qemuBuildHostdevCommandLine(virCommand *cmd,
                            const virDomainDef *def,
                            virQEMUCaps *qemuCaps,
                            unsigned int *bootHostdevNet)
{
    size_t i;

    for (i = 0; i < def->nhostdevs; i++) {
        virDomainHostdevDef *hostdev = def->hostdevs[i];
        virDomainHostdevSubsys *subsys = &hostdev->source.subsys;
        virDomainHostdevSubsysMediatedDev *mdevsrc = &subsys->u.mdev;
        g_autofree char *devstr = NULL;
        g_autofree char *vhostfdName = NULL;
        unsigned int bootIndex = hostdev->info->bootIndex;
        int vhostfd = -1;

        if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;

        switch ((virDomainHostdevSubsysType) subsys->type) {
        /* USB */
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
            virCommandAddArg(cmd, "-device");
            if (!(devstr =
                  qemuBuildUSBHostdevDevStr(def, hostdev, qemuCaps)))
                return -1;
            virCommandAddArg(cmd, devstr);

            break;

        /* PCI */
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            /* bootNet will be non-0 if boot order was set and no other
             * net devices were encountered
             */
            if (hostdev->parentnet && bootIndex == 0) {
                bootIndex = *bootHostdevNet;
                *bootHostdevNet = 0;
            }

           /* Ignore unassigned devices  */
           if (hostdev->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_UNASSIGNED)
               continue;

            if (qemuCommandAddExtDevice(cmd, hostdev->info) < 0)
                return -1;

            virCommandAddArg(cmd, "-device");
            devstr = qemuBuildPCIHostdevDevStr(def, hostdev, bootIndex, qemuCaps);
            if (!devstr)
                return -1;
            virCommandAddArg(cmd, devstr);

            break;

        /* SCSI */
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI:
            if (qemuBuildHostdevSCSICommandLine(cmd, def, hostdev, qemuCaps) < 0)
                return -1;
            break;

        /* SCSI_host */
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST:
            if (hostdev->source.subsys.u.scsi_host.protocol ==
                VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_HOST_PROTOCOL_TYPE_VHOST) {

                if (virSCSIVHostOpenVhostSCSI(&vhostfd) < 0)
                    return -1;

                vhostfdName = g_strdup_printf("%d", vhostfd);

                virCommandPassFD(cmd, vhostfd,
                                 VIR_COMMAND_PASS_FD_CLOSE_PARENT);

                virCommandAddArg(cmd, "-device");
                if (!(devstr = qemuBuildSCSIVHostHostdevDevStr(def,
                                                               hostdev,
                                                               qemuCaps,
                                                               vhostfdName)))
                    return -1;

                virCommandAddArg(cmd, devstr);
            }

            break;

        /* MDEV */
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV:
            switch ((virMediatedDeviceModelType) mdevsrc->model) {
            case VIR_MDEV_MODEL_TYPE_VFIO_PCI:
            case VIR_MDEV_MODEL_TYPE_VFIO_CCW:
            case VIR_MDEV_MODEL_TYPE_VFIO_AP:
                break;
            case VIR_MDEV_MODEL_TYPE_LAST:
            default:
                virReportEnumRangeError(virMediatedDeviceModelType,
                                        subsys->u.mdev.model);
                return -1;
            }

            virCommandAddArg(cmd, "-device");
            if (!(devstr =
                  qemuBuildHostdevMediatedDevStr(def, hostdev, qemuCaps)))
                return -1;
            virCommandAddArg(cmd, devstr);

            break;

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST:
            break;
        }
    }

    return 0;
}


static int
qemuBuildMonitorCommandLine(virLogManager *logManager,
                            virSecurityManager *secManager,
                            virCommand *cmd,
                            virQEMUDriverConfig *cfg,
                            virDomainDef *def,
                            qemuDomainObjPrivate *priv)
{
    g_autofree char *chrdev = NULL;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (priv->chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (!priv->monConfig)
        return 0;

    if (!(chrdev = qemuBuildChrChardevStr(logManager, secManager,
                                          cmd, cfg, def,
                                          priv->monConfig, "monitor",
                                          priv->qemuCaps, cdevflags)))
        return -1;
    virCommandAddArg(cmd, "-chardev");
    virCommandAddArg(cmd, chrdev);

    virCommandAddArg(cmd, "-mon");
    virCommandAddArg(cmd, "chardev=charmonitor,id=monitor,mode=control");

    return 0;
}


static char *
qemuBuildVirtioSerialPortDevStr(const virDomainDef *def,
                                virDomainChrDef *dev)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *contAlias;

    switch (dev->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        virBufferAddLit(&buf, "virtconsole");
        break;
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        virBufferAddLit(&buf, "virtserialport");
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cannot use virtio serial for parallel/serial devices"));
        return NULL;
    }

    if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
        /* Check it's a virtio-serial address */
        if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("virtio serial device has invalid address type"));
            return NULL;
        }

        contAlias = virDomainControllerAliasFind(def, VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL,
                                                 dev->info.addr.vioserial.controller);
        if (!contAlias)
            return NULL;

        virBufferAsprintf(&buf, ",bus=%s.%d,nr=%d", contAlias,
                          dev->info.addr.vioserial.bus,
                          dev->info.addr.vioserial.port);
    }

    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
        dev->source->type == VIR_DOMAIN_CHR_TYPE_SPICEVMC &&
        dev->target.name &&
        STRNEQ(dev->target.name, "com.redhat.spice.0")) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported spicevmc target name '%s'"),
                       dev->target.name);
        return NULL;
    }

    virBufferAsprintf(&buf, ",chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);
    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
        (dev->source->type == VIR_DOMAIN_CHR_TYPE_SPICEVMC ||
         dev->target.name)) {
        virBufferAsprintf(&buf, ",name=%s", dev->target.name
                          ? dev->target.name : "com.redhat.spice.0");
    }

    return virBufferContentAndReset(&buf);
}

static char *
qemuBuildSclpDevStr(virDomainChrDef *dev)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE) {
        switch (dev->targetType) {
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
            virBufferAddLit(&buf, "sclpconsole");
            break;
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
            virBufferAddLit(&buf, "sclplmconsole");
            break;
        }
    } else {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cannot use slcp with devices other than console"));
        return NULL;
    }
    virBufferAsprintf(&buf, ",chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildRNGBackendChrdevStr(virLogManager *logManager,
                             virSecurityManager *secManager,
                             virCommand *cmd,
                             virQEMUDriverConfig *cfg,
                             const virDomainDef *def,
                             virDomainRNGDef *rng,
                             virQEMUCaps *qemuCaps,
                             char **chr,
                             bool chardevStdioLogd)
{
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;

    *chr = NULL;

    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
    case VIR_DOMAIN_RNG_BACKEND_BUILTIN:
    case VIR_DOMAIN_RNG_BACKEND_LAST:
        /* no chardev backend is needed */
        return 0;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
        if (!(*chr = qemuBuildChrChardevStr(logManager, secManager,
                                            cmd, cfg, def,
                                            rng->source.chardev,
                                            rng->info.alias, qemuCaps,
                                            cdevflags)))
            return -1;
        break;
    }

    return 0;
}


int
qemuBuildRNGBackendProps(virDomainRNGDef *rng,
                         virJSONValue **props)
{
    g_autofree char *objAlias = NULL;
    g_autofree char *charBackendAlias = NULL;

    objAlias = g_strdup_printf("obj%s", rng->info.alias);

    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        if (qemuMonitorCreateObjectProps(props, "rng-random", objAlias,
                                         "s:filename", rng->source.file,
                                         NULL) < 0)
            return -1;

        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
        if (!(charBackendAlias = qemuAliasChardevFromDevAlias(rng->info.alias)))
            return -1;

        if (qemuMonitorCreateObjectProps(props, "rng-egd", objAlias,
                                         "s:chardev", charBackendAlias,
                                         NULL) < 0)
            return -1;

        break;

    case VIR_DOMAIN_RNG_BACKEND_BUILTIN:
        if (qemuMonitorCreateObjectProps(props, "rng-builtin", objAlias,
                                         NULL) < 0)
            return -1;

        break;

    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    return 0;
}


char *
qemuBuildRNGDevStr(const virDomainDef *def,
                   virDomainRNGDef *dev,
                   virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (qemuBuildVirtioDevStr(&buf, "virtio-rng", qemuCaps,
                              VIR_DOMAIN_DEVICE_RNG, dev) < 0) {
        return NULL;
    }

    virBufferAsprintf(&buf, ",rng=obj%s,id=%s",
                      dev->info.alias, dev->info.alias);

    if (dev->rate > 0) {
        virBufferAsprintf(&buf, ",max-bytes=%u", dev->rate);
        if (dev->period)
            virBufferAsprintf(&buf, ",period=%u", dev->period);
        else
            virBufferAddLit(&buf, ",period=1000");
    }

    qemuBuildVirtioOptionsStr(&buf, dev->virtio);

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildRNGCommandLine(virLogManager *logManager,
                        virSecurityManager *secManager,
                        virCommand *cmd,
                        virQEMUDriverConfig *cfg,
                        const virDomainDef *def,
                        virQEMUCaps *qemuCaps,
                        bool chardevStdioLogd)
{
    size_t i;

    for (i = 0; i < def->nrngs; i++) {
        g_autoptr(virJSONValue) props = NULL;
        g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
        virDomainRNGDef *rng = def->rngs[i];
        g_autofree char *chardev = NULL;
        g_autofree char *devstr = NULL;
        int rc;

        if (!rng->info.alias) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("RNG device is missing alias"));
            return -1;
        }

        /* possibly add character device for backend */
        if (qemuBuildRNGBackendChrdevStr(logManager, secManager, cmd, cfg, def,
                                         rng, qemuCaps, &chardev,
                                         chardevStdioLogd) < 0)
            return -1;

        if (chardev)
            virCommandAddArgList(cmd, "-chardev", chardev, NULL);

        if (qemuBuildRNGBackendProps(rng, &props) < 0)
            return -1;

        rc = qemuBuildObjectCommandlineFromJSON(&buf, props, qemuCaps);

        if (rc < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);

        /* add the device */
        if (qemuCommandAddExtDevice(cmd, &rng->info) < 0)
            return -1;

        if (!(devstr = qemuBuildRNGDevStr(def, rng, qemuCaps)))
            return -1;
        virCommandAddArgList(cmd, "-device", devstr, NULL);
    }

    return 0;
}


static char *
qemuBuildSmbiosBiosStr(virSysinfoBIOSDef *def)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=0");

    /* 0:Vendor */
    if (def->vendor) {
        virBufferAddLit(&buf, ",vendor=");
        virQEMUBuildBufferEscapeComma(&buf, def->vendor);
    }
    /* 0:BIOS Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 0:BIOS Release Date */
    if (def->date) {
        virBufferAddLit(&buf, ",date=");
        virQEMUBuildBufferEscapeComma(&buf, def->date);
    }
    /* 0:System BIOS Major Release and 0:System BIOS Minor Release */
    if (def->release) {
        virBufferAddLit(&buf, ",release=");
        virQEMUBuildBufferEscapeComma(&buf, def->release);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosSystemStr(virSysinfoSystemDef *def,
                         bool skip_uuid)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!def ||
        (!def->manufacturer && !def->product && !def->version &&
         !def->serial && (!def->uuid || skip_uuid) &&
         def->sku && !def->family))
        return NULL;

    virBufferAddLit(&buf, "type=1");

    /* 1:Manufacturer */
    if (def->manufacturer) {
        virBufferAddLit(&buf, ",manufacturer=");
        virQEMUBuildBufferEscapeComma(&buf, def->manufacturer);
    }
     /* 1:Product Name */
    if (def->product) {
        virBufferAddLit(&buf, ",product=");
        virQEMUBuildBufferEscapeComma(&buf, def->product);
    }
    /* 1:Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 1:Serial Number */
    if (def->serial) {
        virBufferAddLit(&buf, ",serial=");
        virQEMUBuildBufferEscapeComma(&buf, def->serial);
    }
    /* 1:UUID */
    if (def->uuid && !skip_uuid) {
        virBufferAddLit(&buf, ",uuid=");
        virQEMUBuildBufferEscapeComma(&buf, def->uuid);
    }
    /* 1:SKU Number */
    if (def->sku) {
        virBufferAddLit(&buf, ",sku=");
        virQEMUBuildBufferEscapeComma(&buf, def->sku);
    }
    /* 1:Family */
    if (def->family) {
        virBufferAddLit(&buf, ",family=");
        virQEMUBuildBufferEscapeComma(&buf, def->family);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosBaseBoardStr(virSysinfoBaseBoardDef *def)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=2");

    /* 2:Manufacturer */
    virBufferAddLit(&buf, ",manufacturer=");
    virQEMUBuildBufferEscapeComma(&buf, def->manufacturer);
    /* 2:Product Name */
    if (def->product) {
        virBufferAddLit(&buf, ",product=");
        virQEMUBuildBufferEscapeComma(&buf, def->product);
    }
    /* 2:Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 2:Serial Number */
    if (def->serial) {
        virBufferAddLit(&buf, ",serial=");
        virQEMUBuildBufferEscapeComma(&buf, def->serial);
    }
    /* 2:Asset Tag */
    if (def->asset) {
        virBufferAddLit(&buf, ",asset=");
        virQEMUBuildBufferEscapeComma(&buf, def->asset);
    }
    /* 2:Location */
    if (def->location) {
        virBufferAddLit(&buf, ",location=");
        virQEMUBuildBufferEscapeComma(&buf, def->location);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosOEMStringsStr(virSysinfoOEMStringsDef *def)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=11");

    for (i = 0; i < def->nvalues; i++) {
        virBufferAddLit(&buf, ",value=");
        virQEMUBuildBufferEscapeComma(&buf, def->values[i]);
    }

    return virBufferContentAndReset(&buf);
}


static char *
qemuBuildSmbiosChassisStr(virSysinfoChassisDef *def)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!def)
        return NULL;

    virBufferAddLit(&buf, "type=3");

    /* 3:Manufacturer */
    virBufferAddLit(&buf, ",manufacturer=");
    virQEMUBuildBufferEscapeComma(&buf, def->manufacturer);
    /* 3:Version */
    if (def->version) {
        virBufferAddLit(&buf, ",version=");
        virQEMUBuildBufferEscapeComma(&buf, def->version);
    }
    /* 3:Serial Number */
    if (def->serial) {
        virBufferAddLit(&buf, ",serial=");
        virQEMUBuildBufferEscapeComma(&buf, def->serial);
    }
    /* 3:Asset Tag */
    if (def->asset) {
        virBufferAddLit(&buf, ",asset=");
        virQEMUBuildBufferEscapeComma(&buf, def->asset);
    }
    /* 3:Sku */
    if (def->sku) {
        virBufferAddLit(&buf, ",sku=");
        virQEMUBuildBufferEscapeComma(&buf, def->sku);
    }

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildSmbiosCommandLine(virCommand *cmd,
                           virQEMUDriver *driver,
                           const virDomainDef *def)
{
    size_t i;
    virSysinfoDef *source = NULL;
    bool skip_uuid = false;

    if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_NONE ||
        def->os.smbios_mode == VIR_DOMAIN_SMBIOS_EMULATE)
        return 0;

    /* should we really error out or just warn in those cases ? */
    if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_HOST) {
        if (driver->hostsysinfo == NULL) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Host SMBIOS information is not available"));
            return -1;
        }
        source = driver->hostsysinfo;
        /* Host and guest uuid must differ, by definition of UUID. */
        skip_uuid = true;
    } else if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_SYSINFO) {
        for (i = 0; i < def->nsysinfo; i++) {
            if (def->sysinfo[i]->type == VIR_SYSINFO_SMBIOS) {
                source = def->sysinfo[i];
                break;
            }
        }

        if (!source) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Domain '%s' sysinfo are not available"),
                           def->name);
            return -1;
        }
        /* domain_conf guaranteed that system_uuid matches guest uuid. */
    }
    if (source != NULL) {
        char *smbioscmd;

        smbioscmd = qemuBuildSmbiosBiosStr(source->bios);
        if (smbioscmd != NULL) {
            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }
        smbioscmd = qemuBuildSmbiosSystemStr(source->system, skip_uuid);
        if (smbioscmd != NULL) {
            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }

        if (source->nbaseBoard > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("qemu does not support more than "
                             "one entry to Type 2 in SMBIOS table"));
            return -1;
        }

        for (i = 0; i < source->nbaseBoard; i++) {
            if (!(smbioscmd =
                  qemuBuildSmbiosBaseBoardStr(source->baseBoard + i)))
                return -1;

            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }

        smbioscmd = qemuBuildSmbiosChassisStr(source->chassis);
        if (smbioscmd != NULL) {
            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }

        if (source->oemStrings) {
            if (!(smbioscmd = qemuBuildSmbiosOEMStringsStr(source->oemStrings)))
                return -1;

            virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
            VIR_FREE(smbioscmd);
        }
    }

    return 0;
}


static int
qemuBuildSysinfoCommandLine(virCommand *cmd,
                            const virDomainDef *def)
{
    size_t i;

    /* We need to handle VIR_SYSINFO_FWCFG here, because
     * VIR_SYSINFO_SMBIOS is handled in qemuBuildSmbiosCommandLine() */
    for (i = 0; i < def->nsysinfo; i++) {
        size_t j;

        if (def->sysinfo[i]->type != VIR_SYSINFO_FWCFG)
            continue;

        for (j = 0; j < def->sysinfo[i]->nfw_cfgs; j++) {
            const virSysinfoFWCfgDef *f = &def->sysinfo[i]->fw_cfgs[j];
            g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

            virBufferAsprintf(&buf, "name=%s", f->name);

            if (f->value)
                virBufferEscapeString(&buf, ",string=%s", f->value);
            else
                virBufferEscapeString(&buf, ",file=%s", f->file);

            virCommandAddArg(cmd, "-fw_cfg");
            virCommandAddArgBuffer(cmd, &buf);
        }
    }

    return 0;
}


static int
qemuBuildVMGenIDCommandLine(virCommand *cmd,
                            const virDomainDef *def)
{
    g_auto(virBuffer) opts = VIR_BUFFER_INITIALIZER;
    char guid[VIR_UUID_STRING_BUFLEN];

    if (!def->genidRequested)
        return 0;

    virUUIDFormat(def->genid, guid);
    virBufferAsprintf(&opts, "vmgenid,guid=%s,id=vmgenid0", guid);

    virCommandAddArg(cmd, "-device");
    virCommandAddArgBuffer(cmd, &opts);

    return 0;
}


static int
qemuBuildSgaCommandLine(virCommand *cmd,
                        const virDomainDef *def)
{
    /* Serial graphics adapter */
    if (def->os.bios.useserial == VIR_TRISTATE_BOOL_YES)
        virCommandAddArgList(cmd, "-device", "sga", NULL);

    return 0;
}


static char *
qemuBuildClockArgStr(virDomainClockDef *def)
{
    size_t i;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    switch (def->offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_UTC:
        virBufferAddLit(&buf, "base=utc");
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        virBufferAddLit(&buf, "base=localtime");
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE: {
        g_autoptr(GDateTime) now = g_date_time_new_now_utc();
        g_autoptr(GDateTime) then = NULL;
        g_autofree char *thenstr = NULL;

        if (def->data.variable.basis == VIR_DOMAIN_CLOCK_BASIS_LOCALTIME) {
            long localOffset;

            /* in the case of basis='localtime', rather than trying to
             * keep that basis (and associated offset from UTC) in the
             * status and deal with adding in the difference each time
             * there is an RTC_CHANGE event, it is simpler and less
             * error prone to just convert the adjustment an offset
             * from UTC right now (and change the status to
             * "basis='utc' to reflect this). This eliminates
             * potential errors in both RTC_CHANGE events and in
             * migration (in the case that the status of DST, or the
             * timezone of the destination host, changed relative to
             * startup).
             */
            if (virTimeLocalOffsetFromUTC(&localOffset) < 0)
               return NULL;
            def->data.variable.adjustment += localOffset;
            def->data.variable.basis = VIR_DOMAIN_CLOCK_BASIS_UTC;
        }

        then = g_date_time_add_seconds(now, def->data.variable.adjustment);
        thenstr = g_date_time_format(then, "%Y-%m-%dT%H:%M:%S");

        /* when an RTC_CHANGE event is received from qemu, we need to
         * have the adjustment used at domain start time available to
         * compute the new offset from UTC. As this new value is
         * itself stored in def->data.variable.adjustment, we need to
         * save a copy of it now.
        */
        def->data.variable.adjustment0 = def->data.variable.adjustment;

        virBufferAsprintf(&buf, "base=%s", thenstr);
    }   break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported clock offset '%s'"),
                       virDomainClockOffsetTypeToString(def->offset));
        return NULL;
    }

    /* Look for an 'rtc' timer element, and add in appropriate
     * clock= and driftfix= */
    for (i = 0; i < def->ntimers; i++) {
        if (def->timers[i]->name == VIR_DOMAIN_TIMER_NAME_RTC) {
            switch (def->timers[i]->track) {
            case -1: /* unspecified - use hypervisor default */
                break;
            case VIR_DOMAIN_TIMER_TRACK_BOOT:
                return NULL;
            case VIR_DOMAIN_TIMER_TRACK_GUEST:
                virBufferAddLit(&buf, ",clock=vm");
                break;
            case VIR_DOMAIN_TIMER_TRACK_WALL:
                virBufferAddLit(&buf, ",clock=host");
                break;
            case VIR_DOMAIN_TIMER_TRACK_REALTIME:
                virBufferAddLit(&buf, ",clock=rt");
                break;
            }

            switch (def->timers[i]->tickpolicy) {
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                /* This is the default - missed ticks delivered when
                   next scheduled, at normal rate */
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                /* deliver ticks at a faster rate until caught up */
                virBufferAddLit(&buf, ",driftfix=slew");
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                return NULL;
            }
            break; /* no need to check other timers - there is only one rtc */
        }
    }

    return virBufferContentAndReset(&buf);
}


/* NOTE: Building of commands can change def->clock->data.* values, so
 *       virDomainDef is not const here.
 */
static int
qemuBuildClockCommandLine(virCommand *cmd,
                          virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    size_t i;
    g_autofree char *rtcopt = NULL;

    virCommandAddArg(cmd, "-rtc");
    if (!(rtcopt = qemuBuildClockArgStr(&def->clock)))
        return -1;
    virCommandAddArg(cmd, rtcopt);

    if (def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE &&
        def->clock.data.timezone) {
        virCommandAddEnvPair(cmd, "TZ", def->clock.data.timezone);
    }

    for (i = 0; i < def->clock.ntimers; i++) {
        switch ((virDomainTimerNameType)def->clock.timers[i]->name) {
        case VIR_DOMAIN_TIMER_NAME_PLATFORM:
            /* qemuDomainDefValidateClockTimers will handle this
             * error condition  */
            return -1;

        case VIR_DOMAIN_TIMER_NAME_TSC:
        case VIR_DOMAIN_TIMER_NAME_KVMCLOCK:
        case VIR_DOMAIN_TIMER_NAME_HYPERVCLOCK:
        case VIR_DOMAIN_TIMER_NAME_ARMVTIMER:
            /* Timers above are handled when building -cpu.  */
        case VIR_DOMAIN_TIMER_NAME_LAST:
            break;

        case VIR_DOMAIN_TIMER_NAME_RTC:
            /* Already handled in qemuDomainDefValidateClockTimers */
            break;

        case VIR_DOMAIN_TIMER_NAME_PIT:
            switch (def->clock.timers[i]->tickpolicy) {
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                /* delay is the default if we don't have kernel
                   (kvm-pit), otherwise, the default is catchup. */
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM_PIT_TICK_POLICY))
                    virCommandAddArgList(cmd, "-global",
                                         "kvm-pit.lost_tick_policy=delay", NULL);
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                /* Do nothing - qemuDomainDefValidateClockTimers handled
                 * the possible error condition here. */
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM_PIT_TICK_POLICY))
                    virCommandAddArgList(cmd, "-global",
                                         "kvm-pit.lost_tick_policy=discard", NULL);
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
                /* no way to support this mode for pit in qemu */
                return -1;
            }
            break;

        case VIR_DOMAIN_TIMER_NAME_HPET:
            /* the only meaningful attribute for hpet is "present". If
             * present is -1, that means it wasn't specified, and
             * should be left at the default for the
             * hypervisor. "default" when -no-hpet exists is "yes",
             * and when -no-hpet doesn't exist is "no". "confusing"?
             * "yes"! */

            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_HPET)) {
                if (def->clock.timers[i]->present == 0)
                    virCommandAddArg(cmd, "-no-hpet");
            }
            break;
        }
    }

    return 0;
}


static int
qemuBuildPMCommandLine(virCommand *cmd,
                       const virDomainDef *def,
                       qemuDomainObjPrivate *priv)
{
    virQEMUCaps *qemuCaps = priv->qemuCaps;

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_SET_ACTION)) {
        /* with new qemu we always want '-no-shutdown' on startup and we set
         * all the other behaviour later during startup */
        virCommandAddArg(cmd, "-no-shutdown");
    } else {
        if (priv->allowReboot == VIR_TRISTATE_BOOL_NO)
            virCommandAddArg(cmd, "-no-reboot");
        else
            virCommandAddArg(cmd, "-no-shutdown");
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_ACPI)) {
        if (def->features[VIR_DOMAIN_FEATURE_ACPI] != VIR_TRISTATE_SWITCH_ON)
            virCommandAddArg(cmd, "-no-acpi");
    }

    /* We fall back to PIIX4_PM even for q35, since it's what we did
       pre-q35-pm support. QEMU starts up fine (with a warning) if
       mixing PIIX PM and -M q35. Starting to reject things here
       could mean we refuse to start existing configs in the wild.*/
    if (def->pm.s3) {
        const char *pm_object = "PIIX4_PM";

        if (qemuDomainIsQ35(def) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_DISABLE_S3))
            pm_object = "ICH9-LPC";

        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "%s.disable_s3=%d",
                               pm_object, def->pm.s3 == VIR_TRISTATE_BOOL_NO);
    }

    if (def->pm.s4) {
        const char *pm_object = "PIIX4_PM";

        if (qemuDomainIsQ35(def) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_DISABLE_S4))
            pm_object = "ICH9-LPC";

        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "%s.disable_s4=%d",
                               pm_object, def->pm.s4 == VIR_TRISTATE_BOOL_NO);
    }

    return 0;
}


static int
qemuBuildBootCommandLine(virCommand *cmd,
                         const virDomainDef *def,
                         virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) boot_buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *boot_opts_str = NULL;

    if (def->os.bootmenu) {
        if (def->os.bootmenu == VIR_TRISTATE_BOOL_YES)
            virBufferAddLit(&boot_buf, "menu=on,");
        else
            virBufferAddLit(&boot_buf, "menu=off,");
    }

    if (def->os.bios.rt_set) {
        virBufferAsprintf(&boot_buf,
                          "reboot-timeout=%d,",
                          def->os.bios.rt_delay);
    }

    if (def->os.bm_timeout_set)
        virBufferAsprintf(&boot_buf, "splash-time=%u,", def->os.bm_timeout);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOT_STRICT))
        virBufferAddLit(&boot_buf, "strict=on,");

    virBufferTrim(&boot_buf, ",");

    boot_opts_str = virBufferContentAndReset(&boot_buf);
    if (boot_opts_str) {
        virCommandAddArg(cmd, "-boot");
        virCommandAddArg(cmd, boot_opts_str);
    }

    if (def->os.kernel)
        virCommandAddArgList(cmd, "-kernel", def->os.kernel, NULL);
    if (def->os.initrd)
        virCommandAddArgList(cmd, "-initrd", def->os.initrd, NULL);
    if (def->os.cmdline)
        virCommandAddArgList(cmd, "-append", def->os.cmdline, NULL);
    if (def->os.dtb)
        virCommandAddArgList(cmd, "-dtb", def->os.dtb, NULL);
    if (def->os.slic_table) {
        g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
        virCommandAddArg(cmd, "-acpitable");
        virBufferAddLit(&buf, "sig=SLIC,file=");
        virQEMUBuildBufferEscapeComma(&buf, def->os.slic_table);
        virCommandAddArgBuffer(cmd, &buf);
    }

    return 0;
}


static int
qemuBuildIOMMUCommandLine(virCommand *cmd,
                          const virDomainDef *def)
{
    const virDomainIOMMUDef *iommu = def->iommu;

    if (!iommu)
        return 0;

    switch (iommu->model) {
    case VIR_DOMAIN_IOMMU_MODEL_INTEL: {
        g_auto(virBuffer) opts = VIR_BUFFER_INITIALIZER;

        virBufferAddLit(&opts, "intel-iommu");
        if (iommu->intremap != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",intremap=%s",
                              virTristateSwitchTypeToString(iommu->intremap));
        }
        if (iommu->caching_mode != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",caching-mode=%s",
                              virTristateSwitchTypeToString(iommu->caching_mode));
        }
        if (iommu->eim != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",eim=%s",
                              virTristateSwitchTypeToString(iommu->eim));
        }
        if (iommu->iotlb != VIR_TRISTATE_SWITCH_ABSENT) {
            virBufferAsprintf(&opts, ",device-iotlb=%s",
                              virTristateSwitchTypeToString(iommu->iotlb));
        }
        if (iommu->aw_bits > 0)
            virBufferAsprintf(&opts, ",aw-bits=%d", iommu->aw_bits);

        virCommandAddArg(cmd, "-device");
        virCommandAddArgBuffer(cmd, &opts);
        break;
    }

    case VIR_DOMAIN_IOMMU_MODEL_SMMUV3:
        /* There is no -device for SMMUv3, so nothing to be done here */
        return 0;

    case VIR_DOMAIN_IOMMU_MODEL_LAST:
    default:
        virReportEnumRangeError(virDomainIOMMUModel, iommu->model);
        return -1;
    }

    return 0;
}


static int
qemuBuildGlobalControllerCommandLine(virCommand *cmd,
                                     const virDomainDef *def)
{
    size_t i;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDef *cont = def->controllers[i];
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
            cont->opts.pciopts.pcihole64) {
            const char *hoststr = NULL;

            switch (cont->model) {
            case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
                hoststr = "i440FX-pcihost";
                break;

            case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
                hoststr = "q35-pcihost";
                break;

            default:
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("64-bit PCI hole setting is only for root"
                                 " PCI controllers"));
                return -1;
            }

            virCommandAddArg(cmd, "-global");
            virCommandAddArgFormat(cmd, "%s.pci-hole64-size=%luK", hoststr,
                                   cont->opts.pciopts.pcihole64size);
        }
    }

    return 0;
}


static void
qemuBuildCpuFeature(virQEMUCaps *qemuCaps,
                    virBuffer *buf,
                    const char *name,
                    bool state)
{
    name = virQEMUCapsCPUFeatureToQEMU(qemuCaps, name);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_EXPANSION))
        virBufferAsprintf(buf, ",%s=%s", name, state ? "on" : "off");
    else
        virBufferAsprintf(buf, ",%c%s", state ? '+' : '-', name);
}


static int
qemuBuildCpuModelArgStr(virQEMUDriver *driver,
                        const virDomainDef *def,
                        virBuffer *buf,
                        virQEMUCaps *qemuCaps)
{
    size_t i;
    virCPUDef *cpu = def->cpu;

    switch ((virCPUMode) cpu->mode) {
    case VIR_CPU_MODE_HOST_PASSTHROUGH:
    case VIR_CPU_MODE_MAXIMUM:
        if (cpu->mode == VIR_CPU_MODE_MAXIMUM)
            virBufferAddLit(buf, "max");
        else
            virBufferAddLit(buf, "host");

        if (def->os.arch == VIR_ARCH_ARMV7L &&
            driver->hostarch == VIR_ARCH_AARCH64) {
            virBufferAddLit(buf, ",aarch64=off");
        }

        if (cpu->migratable) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_MIGRATABLE)) {
                virBufferAsprintf(buf, ",migratable=%s",
                                  virTristateSwitchTypeToString(cpu->migratable));
            }
        }
        break;

    case VIR_CPU_MODE_HOST_MODEL:
        /* HOST_MODEL is a valid CPU mode for domain XMLs of all archs, meaning
         * that we can't move this validation to parse time. By the time we reach
         * this point, all non-PPC64 archs must have translated the CPU model to
         * something else and set the CPU mode to MODE_CUSTOM.
         */
        if (ARCH_IS_PPC64(def->os.arch)) {
            virBufferAddLit(buf, "host");
            if (cpu->model &&
                !(qemuDomainIsPSeries(def) &&
                  virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_MAX_CPU_COMPAT))) {
                virBufferAsprintf(buf, ",compat=%s", cpu->model);
            }
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected host-model CPU for %s architecture"),
                           virArchToString(def->os.arch));
            return -1;
        }
        break;

    case VIR_CPU_MODE_CUSTOM:
        virBufferAdd(buf, cpu->model, -1);
        break;

    case VIR_CPU_MODE_LAST:
        break;
    }

    if ((ARCH_IS_S390(def->os.arch) || ARCH_IS_ARM(def->os.arch)) &&
        cpu->features &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_EXPANSION)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU features not supported by hypervisor for %s "
                         "architecture"), virArchToString(def->os.arch));
        return -1;
    }

    if (cpu->vendor_id)
        virBufferAsprintf(buf, ",vendor=%s", cpu->vendor_id);

    for (i = 0; i < cpu->nfeatures; i++) {
        switch ((virCPUFeaturePolicy) cpu->features[i].policy) {
        case VIR_CPU_FEATURE_FORCE:
        case VIR_CPU_FEATURE_REQUIRE:
            qemuBuildCpuFeature(qemuCaps, buf, cpu->features[i].name, true);
            break;

        case VIR_CPU_FEATURE_DISABLE:
        case VIR_CPU_FEATURE_FORBID:
            qemuBuildCpuFeature(qemuCaps, buf, cpu->features[i].name, false);
            break;

        case VIR_CPU_FEATURE_OPTIONAL:
        case VIR_CPU_FEATURE_LAST:
            break;
        }
    }

    return 0;
}

static int
qemuBuildCpuCommandLine(virCommand *cmd,
                        virQEMUDriver *driver,
                        const virDomainDef *def,
                        virQEMUCaps *qemuCaps)
{
    virArch hostarch = virArchFromHost();
    g_autofree char *cpu = NULL;
    g_autofree char *cpu_flags = NULL;
    g_auto(virBuffer) cpu_buf = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    if (def->cpu &&
        (def->cpu->mode != VIR_CPU_MODE_CUSTOM || def->cpu->model)) {
        if (qemuBuildCpuModelArgStr(driver, def, &cpu_buf, qemuCaps) < 0)
            return -1;
    } else {
        /*
         * Need to force a 32-bit guest CPU type if
         *
         *  1. guest OS is i686
         *  2. host OS is x86_64
         *  3. emulator is qemu-kvm or kvm
         *
         * Or
         *
         *  1. guest OS is i686
         *  2. emulator is qemu-system-x86_64
         */
        if (def->os.arch == VIR_ARCH_I686 &&
            ((hostarch == VIR_ARCH_X86_64 &&
              strstr(def->emulator, "kvm")) ||
             strstr(def->emulator, "x86_64"))) {
            virBufferAddLit(&cpu_buf, "qemu32");
        }
    }

    /* Handle paravirtual timers  */
    for (i = 0; i < def->clock.ntimers; i++) {
        virDomainTimerDef *timer = def->clock.timers[i];

        switch ((virDomainTimerNameType)timer->name) {
        case VIR_DOMAIN_TIMER_NAME_KVMCLOCK:
            if (timer->present != -1) {
                qemuBuildCpuFeature(qemuCaps, &buf, "kvmclock",
                                    !!timer->present);
            }
            break;
        case VIR_DOMAIN_TIMER_NAME_HYPERVCLOCK:
            if (timer->present == 1)
                virBufferAddLit(&buf, ",hv-time");
            break;
        case VIR_DOMAIN_TIMER_NAME_TSC:
            if (timer->frequency > 0)
                virBufferAsprintf(&buf, ",tsc-frequency=%llu", timer->frequency);
            break;
        case VIR_DOMAIN_TIMER_NAME_ARMVTIMER:
            switch (timer->tickpolicy) {
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                virBufferAddLit(&buf, ",kvm-no-adjvtime=off");
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                virBufferAddLit(&buf, ",kvm-no-adjvtime=on");
                break;
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
                break;
            }
            break;
        case VIR_DOMAIN_TIMER_NAME_PLATFORM:
        case VIR_DOMAIN_TIMER_NAME_PIT:
        case VIR_DOMAIN_TIMER_NAME_RTC:
        case VIR_DOMAIN_TIMER_NAME_HPET:
            break;
        case VIR_DOMAIN_TIMER_NAME_LAST:
        default:
            virReportEnumRangeError(virDomainTimerNameType, timer->name);
            return -1;
        }
    }

    if (def->apic_eoi) {
        qemuBuildCpuFeature(qemuCaps, &buf, "kvm_pv_eoi",
                            def->apic_eoi == VIR_TRISTATE_SWITCH_ON);
    }

    if (def->features[VIR_DOMAIN_FEATURE_PVSPINLOCK]) {
        qemuBuildCpuFeature(qemuCaps, &buf, VIR_CPU_x86_KVM_PV_UNHALT,
                            def->features[VIR_DOMAIN_FEATURE_PVSPINLOCK] == VIR_TRISTATE_SWITCH_ON);
    }

    if (def->features[VIR_DOMAIN_FEATURE_HYPERV] == VIR_TRISTATE_SWITCH_ON) {
        const char *hvPrefix = "hv-";

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CANONICAL_CPU_FEATURES))
            hvPrefix = "hv_";

        for (i = 0; i < VIR_DOMAIN_HYPERV_LAST; i++) {
            switch ((virDomainHyperv) i) {
            case VIR_DOMAIN_HYPERV_RELAXED:
            case VIR_DOMAIN_HYPERV_VAPIC:
            case VIR_DOMAIN_HYPERV_VPINDEX:
            case VIR_DOMAIN_HYPERV_RUNTIME:
            case VIR_DOMAIN_HYPERV_SYNIC:
            case VIR_DOMAIN_HYPERV_STIMER:
            case VIR_DOMAIN_HYPERV_RESET:
            case VIR_DOMAIN_HYPERV_FREQUENCIES:
            case VIR_DOMAIN_HYPERV_REENLIGHTENMENT:
            case VIR_DOMAIN_HYPERV_TLBFLUSH:
            case VIR_DOMAIN_HYPERV_IPI:
            case VIR_DOMAIN_HYPERV_EVMCS:
                if (def->hyperv_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAsprintf(&buf, ",%s%s",
                                      hvPrefix,
                                      virDomainHypervTypeToString(i));
                if ((i == VIR_DOMAIN_HYPERV_STIMER) &&
                    (def->hyperv_stimer_direct == VIR_TRISTATE_SWITCH_ON))
                    virBufferAsprintf(&buf, ",%s", VIR_CPU_x86_HV_STIMER_DIRECT);
                break;

            case VIR_DOMAIN_HYPERV_SPINLOCKS:
                if (def->hyperv_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAsprintf(&buf, ",%s=0x%x",
                                      VIR_CPU_x86_HV_SPINLOCKS,
                                      def->hyperv_spinlocks);
                break;

            case VIR_DOMAIN_HYPERV_VENDOR_ID:
                if (def->hyperv_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAsprintf(&buf, ",hv-vendor-id=%s",
                                      def->hyperv_vendor_id);
                break;

            case VIR_DOMAIN_HYPERV_LAST:
                break;
            }
        }
    }

    for (i = 0; i < def->npanics; i++) {
        if (def->panics[i]->model == VIR_DOMAIN_PANIC_MODEL_HYPERV) {
            virBufferAddLit(&buf, ",hv-crash");
            break;
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_KVM] == VIR_TRISTATE_SWITCH_ON) {
        for (i = 0; i < VIR_DOMAIN_KVM_LAST; i++) {
            switch ((virDomainKVM) i) {
            case VIR_DOMAIN_KVM_HIDDEN:
                if (def->kvm_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAddLit(&buf, ",kvm=off");
                break;

            case VIR_DOMAIN_KVM_DEDICATED:
                if (def->kvm_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAddLit(&buf, ",kvm-hint-dedicated=on");
                break;

            case VIR_DOMAIN_KVM_POLLCONTROL:
                if (def->kvm_features[i] == VIR_TRISTATE_SWITCH_ON)
                    virBufferAddLit(&buf, ",kvm-poll-control=on");
                break;

            case VIR_DOMAIN_KVM_LAST:
                break;
            }
        }
    }

    /* ppc64 guests always have PMU enabled, but the 'pmu' option
     * is not supported. */
    if (def->features[VIR_DOMAIN_FEATURE_PMU] && !ARCH_IS_PPC64(def->os.arch)) {
        virTristateSwitch pmu = def->features[VIR_DOMAIN_FEATURE_PMU];
        virBufferAsprintf(&buf, ",pmu=%s",
                          virTristateSwitchTypeToString(pmu));
    }

    if (def->cpu && def->cpu->cache) {
        virCPUCacheDef *cache = def->cpu->cache;
        bool hostOff = false;
        bool l3Off = false;

        switch (cache->mode) {
        case VIR_CPU_CACHE_MODE_EMULATE:
            virBufferAddLit(&buf, ",l3-cache=on");
            hostOff = true;
            break;

        case VIR_CPU_CACHE_MODE_PASSTHROUGH:
            virBufferAddLit(&buf, ",host-cache-info=on");
            l3Off = true;
            break;

        case VIR_CPU_CACHE_MODE_DISABLE:
            hostOff = l3Off = true;
            break;

        case VIR_CPU_CACHE_MODE_LAST:
            break;
        }

        if (hostOff &&
            (def->cpu->mode == VIR_CPU_MODE_HOST_PASSTHROUGH ||
             def->cpu->mode == VIR_CPU_MODE_MAXIMUM) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_CACHE))
            virBufferAddLit(&buf, ",host-cache-info=off");

        if (l3Off &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_CACHE))
            virBufferAddLit(&buf, ",l3-cache=off");
    }

    cpu = virBufferContentAndReset(&cpu_buf);
    cpu_flags = virBufferContentAndReset(&buf);

    if (cpu_flags && !cpu) {
        const char *default_model;

        switch ((int)def->os.arch) {
        case VIR_ARCH_I686:
            default_model = "qemu32";
            break;
        case VIR_ARCH_X86_64:
            default_model = "qemu64";
            break;
        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("CPU flags requested but can't determine "
                             "default CPU for arch %s"),
                           virArchToString(def->os.arch));
            return -1;
        }

        cpu = g_strdup(default_model);
    }

    if (cpu) {
        virCommandAddArg(cmd, "-cpu");
        virCommandAddArgFormat(cmd, "%s%s", cpu, NULLSTR_EMPTY(cpu_flags));
    }

    return 0;
}


static bool
qemuAppendKeyWrapMachineParm(virBuffer *buf, virQEMUCaps *qemuCaps,
                             virQEMUCapsFlags flag, const char *pname,
                             virTristateSwitch pstate)
{
    if (pstate != VIR_TRISTATE_SWITCH_ABSENT) {
        if (!virQEMUCapsGet(qemuCaps, flag)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("%s is not available with this QEMU binary"), pname);
            return false;
        }

        virBufferAsprintf(buf, ",%s=%s", pname,
                          virTristateSwitchTypeToString(pstate));
    }

    return true;
}

static bool
qemuAppendKeyWrapMachineParms(virBuffer *buf, virQEMUCaps *qemuCaps,
                              const virDomainKeyWrapDef *keywrap)
{
    if (!qemuAppendKeyWrapMachineParm(buf, qemuCaps, QEMU_CAPS_AES_KEY_WRAP,
                                      "aes-key-wrap", keywrap->aes))
        return false;

    if (!qemuAppendKeyWrapMachineParm(buf, qemuCaps, QEMU_CAPS_DEA_KEY_WRAP,
                                      "dea-key-wrap", keywrap->dea))
        return false;

    return true;
}


static void
qemuAppendLoadparmMachineParm(virBuffer *buf,
                              const virDomainDef *def)
{
    size_t i = 0;

    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDef *disk = def->disks[i];

        if (disk->info.bootIndex == 1 && disk->info.loadparm) {
            virBufferAsprintf(buf, ",loadparm=%s", disk->info.loadparm);
            return;
        }
    }

    /* Network boot device */
    for (i = 0; i < def->nnets; i++) {
        virDomainNetDef *net = def->nets[i];

        if (net->info.bootIndex == 1 && net->info.loadparm) {
            virBufferAsprintf(buf, ",loadparm=%s", net->info.loadparm);
            return;
        }
    }
}


static int
qemuBuildNameCommandLine(virCommand *cmd,
                         virQEMUDriverConfig *cfg,
                         const virDomainDef *def)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virCommandAddArg(cmd, "-name");

    /* The 'guest' option let's us handle a name with '=' embedded in it */
    virBufferAddLit(&buf, "guest=");
    virQEMUBuildBufferEscapeComma(&buf, def->name);

    if (cfg->setProcessName)
        virBufferAsprintf(&buf, ",process=qemu:%s", def->name);

    virBufferAddLit(&buf, ",debug-threads=on");

    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}

static int
qemuBuildMachineCommandLine(virCommand *cmd,
                            virQEMUDriverConfig *cfg,
                            const virDomainDef *def,
                            virQEMUCaps *qemuCaps,
                            qemuDomainObjPrivate *priv)
{
    virTristateSwitch vmport = def->features[VIR_DOMAIN_FEATURE_VMPORT];
    virTristateSwitch smm = def->features[VIR_DOMAIN_FEATURE_SMM];
    virCPUDef *cpu = def->cpu;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    virCommandAddArg(cmd, "-machine");
    virBufferAdd(&buf, def->os.machine, -1);

    switch ((virDomainVirtType)def->virtType) {
    case VIR_DOMAIN_VIRT_QEMU:
        virBufferAddLit(&buf, ",accel=tcg");
        break;

    case VIR_DOMAIN_VIRT_KVM:
        virBufferAddLit(&buf, ",accel=kvm");
        break;

    case VIR_DOMAIN_VIRT_KQEMU:
    case VIR_DOMAIN_VIRT_XEN:
    case VIR_DOMAIN_VIRT_LXC:
    case VIR_DOMAIN_VIRT_UML:
    case VIR_DOMAIN_VIRT_OPENVZ:
    case VIR_DOMAIN_VIRT_TEST:
    case VIR_DOMAIN_VIRT_VMWARE:
    case VIR_DOMAIN_VIRT_HYPERV:
    case VIR_DOMAIN_VIRT_VBOX:
    case VIR_DOMAIN_VIRT_PHYP:
    case VIR_DOMAIN_VIRT_PARALLELS:
    case VIR_DOMAIN_VIRT_BHYVE:
    case VIR_DOMAIN_VIRT_VZ:
    case VIR_DOMAIN_VIRT_NONE:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("the QEMU binary does not support %s"),
                       virDomainVirtTypeToString(def->virtType));
        return -1;

    case VIR_DOMAIN_VIRT_LAST:
    default:
        virReportEnumRangeError(virDomainVirtType, def->virtType);
        return -1;
    }

    /* To avoid the collision of creating USB controllers when calling
     * machine->init in QEMU, it needs to set usb=off
     */
    virBufferAddLit(&buf, ",usb=off");

    if (vmport != VIR_TRISTATE_SWITCH_ABSENT)
        virBufferAsprintf(&buf, ",vmport=%s",
                          virTristateSwitchTypeToString(vmport));

    if (smm)
        virBufferAsprintf(&buf, ",smm=%s", virTristateSwitchTypeToString(smm));

    if (def->mem.dump_core) {
        virBufferAsprintf(&buf, ",dump-guest-core=%s",
                          virTristateSwitchTypeToString(def->mem.dump_core));
    } else {
        virBufferAsprintf(&buf, ",dump-guest-core=%s",
                          cfg->dumpGuestCore ? "on" : "off");
    }

    if (def->mem.nosharepages)
        virBufferAddLit(&buf, ",mem-merge=off");

    if (def->keywrap &&
        !qemuAppendKeyWrapMachineParms(&buf, qemuCaps, def->keywrap))
        return -1;

    if (def->features[VIR_DOMAIN_FEATURE_GIC] == VIR_TRISTATE_SWITCH_ON) {
        bool hasGICVersionOption = virQEMUCapsGet(qemuCaps,
                                                  QEMU_CAPS_MACH_VIRT_GIC_VERSION);

        switch ((virGICVersion) def->gic_version) {
        case VIR_GIC_VERSION_2:
            if (!hasGICVersionOption) {
                /* If the gic-version option is not available, we can't
                 * configure the GIC; however, we know that before the
                 * option was introduced the guests would always get a
                 * GICv2, so in order to maintain compatibility with
                 * those old QEMU versions all we need to do is stop
                 * early instead of erroring out */
                break;
            }
            G_GNUC_FALLTHROUGH;

        case VIR_GIC_VERSION_3:
        case VIR_GIC_VERSION_HOST:
            if (!hasGICVersionOption) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("gic-version option is not available "
                                 "with this QEMU binary"));
                return -1;
            }

            virBufferAsprintf(&buf, ",gic-version=%s",
                              virGICVersionTypeToString(def->gic_version));
            break;

        case VIR_GIC_VERSION_NONE:
        case VIR_GIC_VERSION_LAST:
        default:
            break;
        }
    }

    if (def->iommu) {
        switch (def->iommu->model) {
        case VIR_DOMAIN_IOMMU_MODEL_INTEL:
            /* The 'intel' IOMMu is formatted in qemuBuildIOMMUCommandLine */
            break;

        case VIR_DOMAIN_IOMMU_MODEL_SMMUV3:
            virBufferAddLit(&buf, ",iommu=smmuv3");
            break;

        case VIR_DOMAIN_IOMMU_MODEL_LAST:
        default:
            virReportEnumRangeError(virDomainIOMMUModel, def->iommu->model);
            return -1;
        }
    }

    for (i = 0; i < def->nmems; i++) {
        if (def->mems[i]->model == VIR_DOMAIN_MEMORY_MODEL_NVDIMM) {
            virBufferAddLit(&buf, ",nvdimm=on");
            break;
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_IOAPIC] != VIR_DOMAIN_IOAPIC_NONE) {
        switch ((virDomainIOAPIC) def->features[VIR_DOMAIN_FEATURE_IOAPIC]) {
        case VIR_DOMAIN_IOAPIC_QEMU:
            virBufferAddLit(&buf, ",kernel_irqchip=split");
            break;
        case VIR_DOMAIN_IOAPIC_KVM:
            virBufferAddLit(&buf, ",kernel_irqchip=on");
            break;
        case VIR_DOMAIN_IOAPIC_NONE:
        case VIR_DOMAIN_IOAPIC_LAST:
            break;
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_HPT] == VIR_TRISTATE_SWITCH_ON) {

        if (def->hpt_resizing != VIR_DOMAIN_HPT_RESIZING_NONE) {
            virBufferAsprintf(&buf, ",resize-hpt=%s",
                              virDomainHPTResizingTypeToString(def->hpt_resizing));
        }

        if (def->hpt_maxpagesize > 0) {
            virBufferAsprintf(&buf, ",cap-hpt-max-page-size=%lluk",
                              def->hpt_maxpagesize);
        }
    }

    if (def->features[VIR_DOMAIN_FEATURE_HTM] != VIR_TRISTATE_SWITCH_ABSENT) {
        const char *str;
        str = virTristateSwitchTypeToString(def->features[VIR_DOMAIN_FEATURE_HTM]);
        virBufferAsprintf(&buf, ",cap-htm=%s", str);
    }

    if (def->features[VIR_DOMAIN_FEATURE_NESTED_HV] != VIR_TRISTATE_SWITCH_ABSENT) {
        const char *str;
        str = virTristateSwitchTypeToString(def->features[VIR_DOMAIN_FEATURE_NESTED_HV]);
        virBufferAsprintf(&buf, ",cap-nested-hv=%s", str);
    }

    if (def->features[VIR_DOMAIN_FEATURE_CCF_ASSIST] != VIR_TRISTATE_SWITCH_ABSENT) {
        const char *str;
        str = virTristateSwitchTypeToString(def->features[VIR_DOMAIN_FEATURE_CCF_ASSIST]);
        virBufferAsprintf(&buf, ",cap-ccf-assist=%s", str);
    }

    if (def->features[VIR_DOMAIN_FEATURE_CFPC] != VIR_DOMAIN_CFPC_NONE) {
        const char *str = virDomainCFPCTypeToString(def->features[VIR_DOMAIN_FEATURE_CFPC]);
        virBufferAsprintf(&buf, ",cap-cfpc=%s", str);
    }

    if (def->features[VIR_DOMAIN_FEATURE_SBBC] != VIR_DOMAIN_SBBC_NONE) {
        const char *str = virDomainSBBCTypeToString(def->features[VIR_DOMAIN_FEATURE_SBBC]);
        virBufferAsprintf(&buf, ",cap-sbbc=%s", str);
    }

    if (def->features[VIR_DOMAIN_FEATURE_IBS] != VIR_DOMAIN_IBS_NONE) {
        const char *str = virDomainIBSTypeToString(def->features[VIR_DOMAIN_FEATURE_IBS]);
        virBufferAsprintf(&buf, ",cap-ibs=%s", str);
    }

    if (cpu && cpu->model &&
        cpu->mode == VIR_CPU_MODE_HOST_MODEL &&
        qemuDomainIsPSeries(def) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_PSERIES_MAX_CPU_COMPAT)) {
        virBufferAsprintf(&buf, ",max-cpu-compat=%s", cpu->model);
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_LOADPARM))
        qemuAppendLoadparmMachineParm(&buf, def);

    if (def->sec) {
        switch ((virDomainLaunchSecurity) def->sec->sectype) {
        case VIR_DOMAIN_LAUNCH_SECURITY_SEV:
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_CONFIDENTAL_GUEST_SUPPORT)) {
                virBufferAddLit(&buf, ",confidential-guest-support=lsec0");
            } else {
                virBufferAddLit(&buf, ",memory-encryption=lsec0");
            }
            break;
        case VIR_DOMAIN_LAUNCH_SECURITY_PV:
            virBufferAddLit(&buf, ",confidential-guest-support=lsec0");
            break;
        case VIR_DOMAIN_LAUNCH_SECURITY_NONE:
        case VIR_DOMAIN_LAUNCH_SECURITY_LAST:
            virReportEnumRangeError(virDomainLaunchSecurity, def->sec->sectype);
            return -1;
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
        if (priv->pflash0)
            virBufferAsprintf(&buf, ",pflash0=%s", priv->pflash0->nodeformat);
        if (priv->pflash1)
            virBufferAsprintf(&buf, ",pflash1=%s", priv->pflash1->nodeformat);
    }

    if (virDomainNumaHasHMAT(def->numa))
        virBufferAddLit(&buf, ",hmat=on");

    if (!virDomainNumaGetNodeCount(def->numa)) {
        const char *defaultRAMid = NULL;

        /* QEMU is obsoleting -mem-path and -mem-prealloc. That means we have
         * to switch to memory-backend-* even for regular RAM and to keep
         * domain migratable we have to set the same ID as older QEMUs would.
         * If domain has no NUMA nodes and QEMU is new enough to expose ID of
         * the default RAM we want to use it for default RAM (construct
         * memory-backend-* with corresponding attributes instead of obsolete
         * -mem-path and -mem-prealloc).
         * This generates only reference for the memory-backend-* object added
         * later in qemuBuildMemCommandLine() */
        defaultRAMid = virQEMUCapsGetMachineDefaultRAMid(qemuCaps,
                                                         def->virtType,
                                                         def->os.machine);
        if (defaultRAMid)
            virBufferAsprintf(&buf, ",memory-backend=%s", defaultRAMid);
    }

    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


static void
qemuBuildTSEGCommandLine(virCommand *cmd,
                         const virDomainDef *def)
{
    if (!def->tseg_specified)
        return;

    virCommandAddArg(cmd, "-global");

    /* PostParse callback guarantees that the size is divisible by 1 MiB */
    virCommandAddArgFormat(cmd, "mch.extended-tseg-mbytes=%llu",
                           def->tseg_size >> 20);
}


static int
qemuBuildSmpCommandLine(virCommand *cmd,
                        virDomainDef *def,
                        virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    unsigned int maxvcpus = virDomainDefGetVcpusMax(def);
    unsigned int nvcpus = 0;
    virDomainVcpuDef *vcpu;
    size_t i;

    /* count non-hotpluggable enabled vcpus. Hotpluggable ones will be added
     * in a different way */
    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(def, i);
        if (vcpu->online && vcpu->hotpluggable == VIR_TRISTATE_BOOL_NO)
            nvcpus++;
    }

    virCommandAddArg(cmd, "-smp");

    virBufferAsprintf(&buf, "%u", nvcpus);

    if (nvcpus != maxvcpus)
        virBufferAsprintf(&buf, ",maxcpus=%u", maxvcpus);
    /* sockets, cores, and threads are either all zero
     * or all non-zero, thus checking one of them is enough */
    if (def->cpu && def->cpu->sockets) {
        if (def->cpu->dies != 1 && !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SMP_DIES)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Only 1 die per socket is supported"));
            return -1;
        }
        virBufferAsprintf(&buf, ",sockets=%u", def->cpu->sockets);
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SMP_DIES))
            virBufferAsprintf(&buf, ",dies=%u", def->cpu->dies);
        virBufferAsprintf(&buf, ",cores=%u", def->cpu->cores);
        virBufferAsprintf(&buf, ",threads=%u", def->cpu->threads);
    } else {
        virBufferAsprintf(&buf, ",sockets=%u", virDomainDefGetVcpusMax(def));
        virBufferAsprintf(&buf, ",cores=%u", 1);
        virBufferAsprintf(&buf, ",threads=%u", 1);
    }

    virCommandAddArgBuffer(cmd, &buf);
    return 0;
}


static int
qemuBuildMemPathStr(const virDomainDef *def,
                    virCommand *cmd,
                    qemuDomainObjPrivate *priv)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(priv->driver);
    const long system_page_size = virGetSystemPageSizeKB();
    g_autofree char *mem_path = NULL;

    /* There are two cases where we want to put -mem-path onto
     * the command line: First one is when there are no guest
     * NUMA nodes and hugepages are configured. The second one is
     * if user requested file allocation. */
    if (def->mem.nhugepages &&
        def->mem.hugepages[0].size != system_page_size) {
        unsigned long long pagesize = def->mem.hugepages[0].size;
        if (!pagesize &&
            qemuBuildMemoryGetDefaultPagesize(cfg, &pagesize) < 0)
            return -1;
        if (qemuGetDomainHupageMemPath(priv->driver, def, pagesize, &mem_path) < 0)
            return -1;
    } else if (def->mem.source == VIR_DOMAIN_MEMORY_SOURCE_FILE) {
        if (qemuGetMemoryBackingPath(priv->driver, def, "ram", &mem_path) < 0)
            return -1;
    } else {
        return 0;
    }

    if (def->mem.allocation != VIR_DOMAIN_MEMORY_ALLOCATION_IMMEDIATE) {
        virCommandAddArgList(cmd, "-mem-prealloc", NULL);
        priv->memPrealloc = true;
    }

    virCommandAddArgList(cmd, "-mem-path", mem_path, NULL);

    return 0;
}


static int
qemuBuildMemCommandLineMemoryDefaultBackend(virCommand *cmd,
                                            const virDomainDef *def,
                                            qemuDomainObjPrivate *priv,
                                            const char *defaultRAMid)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(priv->driver);
    g_autoptr(virJSONValue) props = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virDomainMemoryDef mem = { 0 };

    mem.size = virDomainDefGetMemoryInitial(def);
    mem.targetNode = -1;
    mem.info.alias = (char *) defaultRAMid;

    if (qemuBuildMemoryBackendProps(&props, defaultRAMid, cfg,
                                    priv, def, &mem, false, true) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, priv->qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);
    return 0;
}


static int
qemuBuildMemCommandLine(virCommand *cmd,
                        const virDomainDef *def,
                        virQEMUCaps *qemuCaps,
                        qemuDomainObjPrivate *priv)
{
    const char *defaultRAMid = NULL;

    virCommandAddArg(cmd, "-m");

    if (virDomainDefHasMemoryHotplug(def)) {
        /* Use the 'k' suffix to let qemu handle the units */
        virCommandAddArgFormat(cmd, "size=%lluk,slots=%u,maxmem=%lluk",
                               virDomainDefGetMemoryInitial(def),
                               def->mem.memory_slots,
                               def->mem.max_memory);

    } else {
       virCommandAddArgFormat(cmd, "%llu",
                              virDomainDefGetMemoryInitial(def) / 1024);
    }

    defaultRAMid = virQEMUCapsGetMachineDefaultRAMid(qemuCaps,
                                                     def->virtType,
                                                     def->os.machine);

    if (defaultRAMid) {
        /* As documented in qemuBuildMachineCommandLine() if QEMU is new enough
         * to expose default RAM ID we must use memory-backend-* even for
         * regular memory because -mem-path and -mem-prealloc are obsolete.
         * However, if domain has one or more NUMA nodes then there is no
         * default RAM and we mustn't generate the memory object. */
        if (!virDomainNumaGetNodeCount(def->numa))
            qemuBuildMemCommandLineMemoryDefaultBackend(cmd, def, priv, defaultRAMid);
    } else {
        if (def->mem.allocation == VIR_DOMAIN_MEMORY_ALLOCATION_IMMEDIATE) {
            virCommandAddArgList(cmd, "-mem-prealloc", NULL);
            priv->memPrealloc = true;
        }

        /*
         * Add '-mem-path' (and '-mem-prealloc') parameter here if
         * the hugepages and no numa node is specified.
         */
        if (!virDomainNumaGetNodeCount(def->numa) &&
            qemuBuildMemPathStr(def, cmd, priv) < 0)
            return -1;
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_OVERCOMMIT)) {
        virCommandAddArg(cmd, "-overcommit");
        virCommandAddArgFormat(cmd, "mem-lock=%s", def->mem.locked ? "on" : "off");
    } else {
        virCommandAddArg(cmd, "-realtime");
        virCommandAddArgFormat(cmd, "mlock=%s",
                               def->mem.locked ? "on" : "off");
    }

    return 0;
}


static int
qemuBuildIOThreadCommandLine(virCommand *cmd,
                             const virDomainDef *def,
                             virQEMUCaps *qemuCaps)
{
    size_t i;

    if (def->niothreadids == 0)
        return 0;

    for (i = 0; i < def->niothreadids; i++) {
        g_autoptr(virJSONValue) props = NULL;
        g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
        g_autofree char *alias = g_strdup_printf("iothread%u", def->iothreadids[i]->iothread_id);

        if (qemuMonitorCreateObjectProps(&props, "iothread", alias, NULL) < 0)
            return -1;

        if (qemuBuildObjectCommandlineFromJSON(&buf, props, qemuCaps) < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);
    }

    return 0;
}


static int
qemuBuilNumaCellCache(virCommand *cmd,
                      const virDomainDef *def,
                      size_t cell)
{
    size_t ncaches = virDomainNumaGetNodeCacheCount(def->numa, cell);
    size_t i;

    if (ncaches == 0)
        return 0;

    for (i = 0; i < ncaches; i++) {
        g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
        unsigned int level;
        unsigned int size;
        unsigned int line;
        virNumaCacheAssociativity associativity;
        virNumaCachePolicy policy;

        if (virDomainNumaGetNodeCache(def->numa, cell, i,
                                      &level, &size, &line,
                                      &associativity, &policy) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unable to format NUMA node cache"));
            return -1;
        }

        virBufferAsprintf(&buf,
                          "hmat-cache,node-id=%zu,size=%uK,level=%u",
                          cell, size, level);

        switch (associativity) {
        case VIR_NUMA_CACHE_ASSOCIATIVITY_NONE:
            virBufferAddLit(&buf, ",associativity=none");
            break;
        case VIR_NUMA_CACHE_ASSOCIATIVITY_DIRECT:
            virBufferAddLit(&buf, ",associativity=direct");
            break;
        case VIR_NUMA_CACHE_ASSOCIATIVITY_FULL:
            virBufferAddLit(&buf, ",associativity=complex");
            break;
        case VIR_NUMA_CACHE_ASSOCIATIVITY_LAST:
            break;
        }

        switch (policy) {
        case VIR_NUMA_CACHE_POLICY_NONE:
            virBufferAddLit(&buf, ",policy=none");
            break;
        case VIR_NUMA_CACHE_POLICY_WRITEBACK:
            virBufferAddLit(&buf, ",policy=write-back");
            break;
        case VIR_NUMA_CACHE_POLICY_WRITETHROUGH:
            virBufferAddLit(&buf, ",policy=write-through");
            break;
        case VIR_NUMA_CACHE_POLICY_LAST:
            break;
        }

        if (line > 0)
            virBufferAsprintf(&buf, ",line=%u", line);

        virCommandAddArg(cmd, "-numa");
        virCommandAddArgBuffer(cmd, &buf);
    }

    return 0;
}


VIR_ENUM_DECL(qemuDomainMemoryHierarchy);
VIR_ENUM_IMPL(qemuDomainMemoryHierarchy,
              4, /* Maximum level of cache */
              "memory", /* Special case, whole memory not specific cache */
              "first-level",
              "second-level",
              "third-level");

static int
qemuBuildNumaHMATCommandLine(virCommand *cmd,
                             const virDomainDef *def)
{
    size_t nlatencies;
    size_t i;

    if (!def->numa)
        return 0;

    nlatencies = virDomainNumaGetInterconnectsCount(def->numa);
    for (i = 0; i < nlatencies; i++) {
        g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
        virNumaInterconnectType type;
        unsigned int initiator;
        unsigned int target;
        unsigned int cache;
        virMemoryLatency accessType;
        unsigned long value;
        const char *hierarchyStr;
        const char *accessStr;

        if (virDomainNumaGetInterconnect(def->numa, i,
                                         &type, &initiator, &target,
                                         &cache, &accessType, &value) < 0)
            return -1;

        hierarchyStr = qemuDomainMemoryHierarchyTypeToString(cache);
        accessStr = virMemoryLatencyTypeToString(accessType);
        virBufferAsprintf(&buf,
                          "hmat-lb,initiator=%u,target=%u,hierarchy=%s,data-type=%s-",
                          initiator, target, hierarchyStr, accessStr);

        switch (type) {
        case VIR_NUMA_INTERCONNECT_TYPE_LATENCY:
            virBufferAsprintf(&buf, "latency,latency=%lu", value);
            break;
        case VIR_NUMA_INTERCONNECT_TYPE_BANDWIDTH:
            virBufferAsprintf(&buf, "bandwidth,bandwidth=%luK", value);
            break;
        }

        virCommandAddArg(cmd, "-numa");
        virCommandAddArgBuffer(cmd, &buf);
    }

    return 0;
}


static int
qemuBuildNumaCommandLine(virQEMUDriverConfig *cfg,
                         virDomainDef *def,
                         virCommand *cmd,
                         qemuDomainObjPrivate *priv)
{
    size_t i, j;
    virQEMUCaps *qemuCaps = priv->qemuCaps;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    char *next = NULL;
    virBuffer *nodeBackends = NULL;
    bool needBackend = false;
    bool hmat = false;
    int rc;
    int ret = -1;
    size_t ncells = virDomainNumaGetNodeCount(def->numa);
    ssize_t masterInitiator = -1;

    if (!virDomainNumatuneNodesetIsAvailable(def->numa, priv->autoNodeset))
        goto cleanup;

    if (!virQEMUCapsGetMachineNumaMemSupported(qemuCaps,
                                               def->virtType,
                                               def->os.machine))
        needBackend = true;

    if (virDomainNumaHasHMAT(def->numa)) {
        needBackend = true;
        hmat = true;
    }

    nodeBackends = g_new0(virBuffer, ncells);

    /* using of -numa memdev= cannot be combined with -numa mem=, thus we
     * need to check which approach to use */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_RAM) ||
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_FILE) ||
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_MEMORY_MEMFD)) {

        for (i = 0; i < ncells; i++) {
            if ((rc = qemuBuildMemoryCellBackendStr(def, cfg, i, priv,
                                                    &nodeBackends[i])) < 0)
                goto cleanup;

            if (rc == 0)
                needBackend = true;
        }
    }

    if (!needBackend &&
        qemuBuildMemPathStr(def, cmd, priv) < 0)
        goto cleanup;

    for (i = 0; i < ncells; i++) {
        if (virDomainNumaGetNodeCpumask(def->numa, i)) {
            masterInitiator = i;
            break;
        }
    }

    if (masterInitiator < 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("At least one NUMA node has to have CPUs"));
        goto cleanup;
    }

    for (i = 0; i < ncells; i++) {
        virBitmap *cpumask = virDomainNumaGetNodeCpumask(def->numa, i);
        ssize_t initiator = virDomainNumaGetNodeInitiator(def->numa, i);

        if (needBackend) {
            virCommandAddArg(cmd, "-object");
            virCommandAddArgBuffer(cmd, &nodeBackends[i]);
        }

        virCommandAddArg(cmd, "-numa");
        virBufferAsprintf(&buf, "node,nodeid=%zu", i);

        if (cpumask) {
            g_autofree char *cpumaskStr = NULL;
            char *tmpmask;

            if (!(cpumaskStr = virBitmapFormat(cpumask)))
                goto cleanup;

            for (tmpmask = cpumaskStr; tmpmask; tmpmask = next) {
                if ((next = strchr(tmpmask, ',')))
                    *(next++) = '\0';
                virBufferAddLit(&buf, ",cpus=");
                virBufferAdd(&buf, tmpmask, -1);
            }
        }

        if (hmat) {
            if (initiator < 0)
                initiator = masterInitiator;

            virBufferAsprintf(&buf, ",initiator=%zd", initiator);
        }

        if (needBackend)
            virBufferAsprintf(&buf, ",memdev=ram-node%zu", i);
        else
            virBufferAsprintf(&buf, ",mem=%llu",
                              virDomainNumaGetNodeMemorySize(def->numa, i) / 1024);

        virCommandAddArgBuffer(cmd, &buf);
    }

    /* If NUMA node distance is specified for at least one pair
     * of nodes, we have to specify all the distances. Even
     * though they might be the default ones. */
    if (virDomainNumaNodesDistancesAreBeingSet(def->numa)) {
        for (i = 0; i < ncells; i++) {
            for (j = 0; j < ncells; j++) {
                size_t distance = virDomainNumaGetNodeDistance(def->numa, i, j);

                virCommandAddArg(cmd, "-numa");
                virBufferAsprintf(&buf, "dist,src=%zu,dst=%zu,val=%zu", i, j, distance);

                virCommandAddArgBuffer(cmd, &buf);
            }
        }
    }

    if (hmat) {
        if (qemuBuildNumaHMATCommandLine(cmd, def) < 0)
            goto cleanup;

        /* This can't be moved into any of the loops above,
         * because hmat-cache can be specified only after hmat-lb. */
        for (i = 0; i < ncells; i++) {
            if (qemuBuilNumaCellCache(cmd, def, i) < 0)
                goto cleanup;
        }
    }

    ret = 0;

 cleanup:
    if (nodeBackends) {
        for (i = 0; i < ncells; i++)
            virBufferFreeAndReset(&nodeBackends[i]);

        VIR_FREE(nodeBackends);
    }

    return ret;
}


static int
qemuBuildMemoryDeviceCommandLine(virCommand *cmd,
                                 virQEMUDriverConfig *cfg,
                                 virDomainDef *def,
                                 qemuDomainObjPrivate *priv)
{
    size_t i;

    /* memory hotplug requires NUMA to be enabled - we already checked
     * that memory devices are present only when NUMA is */
    for (i = 0; i < def->nmems; i++) {
        g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
        char *dimmStr;

        if (qemuBuildMemoryDimmBackendStr(&buf, def->mems[i], def, cfg, priv) < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);

        if (!(dimmStr = qemuBuildMemoryDeviceStr(def, def->mems[i], priv->qemuCaps)))
            return -1;

        virCommandAddArgList(cmd, "-device", dimmStr, NULL);

        VIR_FREE(dimmStr);
    }

    return 0;
}

static void
qemuBuildAudioCommonArg(virBuffer *buf,
                        const char *prefix,
                        virDomainAudioIOCommon *def)
{
    if (def->mixingEngine)
        virBufferAsprintf(buf, ",%s.mixing-engine=%s", prefix,
                          virTristateSwitchTypeToString(def->mixingEngine));
    if (def->fixedSettings)
        virBufferAsprintf(buf, ",%s.fixed-settings=%s", prefix,
                          virTristateSwitchTypeToString(def->fixedSettings));

    if (def->voices)
        virBufferAsprintf(buf, ",%s.voices=%u", prefix, def->voices);
    if (def->bufferLength)
        virBufferAsprintf(buf, ",%s.buffer-length=%u", prefix, def->bufferLength);

    if (def->fixedSettings) {
        if (def->frequency)
            virBufferAsprintf(buf, ",%s.frequency=%u", prefix, def->frequency);
        if (def->channels)
            virBufferAsprintf(buf, ",%s.channels=%u", prefix, def->channels);
        if (def->format)
            virBufferAsprintf(buf, ",%s.format=%s", prefix,
                              virDomainAudioFormatTypeToString(def->format));
    }
}

static void
qemuBuildAudioALSAArg(virBuffer *buf,
                      const char *prefix,
                      virDomainAudioIOALSA *def)
{
    if (def->dev)
        virBufferAsprintf(buf, ",%s.dev=%s", prefix, def->dev);
}

static void
qemuBuildAudioCoreAudioArg(virBuffer *buf,
                           const char *prefix,
                           virDomainAudioIOCoreAudio *def)
{
    if (def->bufferCount)
        virBufferAsprintf(buf, ",%s.buffer-count=%u", prefix, def->bufferCount);
}

static void
qemuBuildAudioJackArg(virBuffer *buf,
                      const char *prefix,
                      virDomainAudioIOJack *def)
{
    if (def->serverName)
        virBufferAsprintf(buf, ",%s.server-name=%s", prefix, def->serverName);
    if (def->clientName)
        virBufferAsprintf(buf, ",%s.client-name=%s", prefix, def->clientName);
    if (def->connectPorts)
        virBufferAsprintf(buf, ",%s.connect-ports=%s", prefix, def->connectPorts);
    if (def->exactName)
        virBufferAsprintf(buf, ",%s.exact-name=%s", prefix,
                          virTristateSwitchTypeToString(def->exactName));
}

static void
qemuBuildAudioOSSArg(virBuffer *buf,
                     const char *prefix,
                     virDomainAudioIOOSS *def)
{
    if (def->dev)
        virBufferAsprintf(buf, ",%s.dev=%s", prefix, def->dev);
    if (def->bufferCount)
        virBufferAsprintf(buf, ",%s.buffer-count=%u", prefix, def->bufferCount);
    if (def->tryPoll)
        virBufferAsprintf(buf, ",%s.try-poll=%s", prefix,
                          virTristateSwitchTypeToString(def->tryPoll));
}

static void
qemuBuildAudioPulseAudioArg(virBuffer *buf,
                            const char *prefix,
                            virDomainAudioIOPulseAudio *def)
{
    if (def->name)
        virBufferAsprintf(buf, ",%s.name=%s", prefix, def->name);
    if (def->streamName)
        virBufferAsprintf(buf, ",%s.stream-name=%s", prefix, def->streamName);
    if (def->latency)
        virBufferAsprintf(buf, ",%s.latency=%u", prefix, def->latency);
}

static void
qemuBuildAudioSDLArg(virBuffer *buf,
                     const char *prefix,
                     virDomainAudioIOSDL *def)
{
    if (def->bufferCount)
        virBufferAsprintf(buf, ",%s.buffer-count=%u", prefix, def->bufferCount);
}

static int
qemuBuildAudioCommandLineArg(virCommand *cmd,
                             virDomainAudioDef *def)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virCommandAddArg(cmd, "-audiodev");

    virBufferAsprintf(&buf, "id=audio%d,driver=%s",
                      def->id,
                      qemuAudioDriverTypeToString(def->type));

    if (def->timerPeriod)
        virBufferAsprintf(&buf, ",timer-period=%u",
                          def->timerPeriod);

    qemuBuildAudioCommonArg(&buf, "in", &def->input);
    qemuBuildAudioCommonArg(&buf, "out", &def->output);

    switch (def->type) {
    case VIR_DOMAIN_AUDIO_TYPE_NONE:
        break;

    case VIR_DOMAIN_AUDIO_TYPE_ALSA:
        qemuBuildAudioALSAArg(&buf, "in", &def->backend.alsa.input);
        qemuBuildAudioALSAArg(&buf, "out", &def->backend.alsa.output);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_COREAUDIO:
        qemuBuildAudioCoreAudioArg(&buf, "in", &def->backend.coreaudio.input);
        qemuBuildAudioCoreAudioArg(&buf, "out", &def->backend.coreaudio.output);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_JACK:
        qemuBuildAudioJackArg(&buf, "in", &def->backend.jack.input);
        qemuBuildAudioJackArg(&buf, "out", &def->backend.jack.output);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_OSS:
        qemuBuildAudioOSSArg(&buf, "in", &def->backend.oss.input);
        qemuBuildAudioOSSArg(&buf, "out", &def->backend.oss.output);

        if (def->backend.oss.tryMMap)
            virBufferAsprintf(&buf, ",try-mmap=%s",
                              virTristateSwitchTypeToString(def->backend.oss.tryMMap));
        if (def->backend.oss.exclusive)
            virBufferAsprintf(&buf, ",exclusive=%s",
                              virTristateSwitchTypeToString(def->backend.oss.exclusive));
        if (def->backend.oss.dspPolicySet)
            virBufferAsprintf(&buf, ",dsp-policy=%d", def->backend.oss.dspPolicy);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_PULSEAUDIO:
        qemuBuildAudioPulseAudioArg(&buf, "in", &def->backend.pulseaudio.input);
        qemuBuildAudioPulseAudioArg(&buf, "out", &def->backend.pulseaudio.output);

        if (def->backend.pulseaudio.serverName)
            virBufferAsprintf(&buf, ",server=%s", def->backend.pulseaudio.serverName);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_SDL:
        qemuBuildAudioSDLArg(&buf, "in", &def->backend.sdl.input);
        qemuBuildAudioSDLArg(&buf, "out", &def->backend.sdl.output);

        if (def->backend.sdl.driver) {
            /*
             * Some SDL audio driver names are different on SDL 1.2
             * vs 2.0.  Given how old SDL 1.2 is, we're not going
             * make any attempt to support it here as it is unlikely
             * to have an real world users. We can assume libvirt
             * driver name strings match SDL 2.0 names.
             */
            virCommandAddEnvPair(cmd, "SDL_AUDIODRIVER",
                                 virDomainAudioSDLDriverTypeToString(
                                     def->backend.sdl.driver));
        }
        break;

    case VIR_DOMAIN_AUDIO_TYPE_SPICE:
        break;

    case VIR_DOMAIN_AUDIO_TYPE_FILE:
        if (def->backend.file.path)
            virBufferEscapeString(&buf, ",path=%s", def->backend.file.path);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainAudioType, def->type);
        return -1;
    }

    virCommandAddArgBuffer(cmd, &buf);
    return 0;
}

static int
qemuBuildAudioCommandLineArgs(virCommand *cmd,
                              virDomainDef *def)
{
    size_t i;
    for (i = 0; i < def->naudios; i++) {
        if (qemuBuildAudioCommandLineArg(cmd, def->audios[i]) < 0)
            return -1;
    }
    return 0;
}

static void
qemuBuildAudioCommonEnv(virCommand *cmd,
                        const char *prefix,
                        virDomainAudioIOCommon *def)
{
    if (def->fixedSettings)
        virCommandAddEnvFormat(cmd, "%sFIXED_SETTINGS=%s",
                               prefix,
                               virTristateSwitchTypeToString(def->fixedSettings));

    if (def->voices)
        virCommandAddEnvFormat(cmd, "%sVOICES=%u",
                               prefix, def->voices);

    if (def->fixedSettings) {
        if (def->frequency)
            virCommandAddEnvFormat(cmd, "%sFIXED_FREQ=%u",
                                   prefix, def->frequency);
        if (def->channels)
            virCommandAddEnvFormat(cmd, "%sFIXED_CHANNELS=%u",
                                   prefix, def->channels);
        if (def->format)
            virCommandAddEnvFormat(cmd, "%sFIXED_FMT=%s",
                                   prefix,
                                   virDomainAudioFormatTypeToString(def->format));
    }
}

static void
qemuBuildAudioALSAEnv(virCommand *cmd,
                      const char *prefix,
                      virDomainAudioIOALSA *def)
{
    if (def->dev)
        virCommandAddEnvFormat(cmd, "%sDEV=%s",
                               prefix, def->dev);
}

static void
qemuBuildAudioCoreAudioEnv(virCommand *cmd,
                           virDomainAudioDef *def)
{
    if (def->backend.coreaudio.output.bufferCount)
        virCommandAddEnvFormat(cmd, "QEMU_COREAUDIO_BUFFER_COUNT=%u",
                               def->backend.coreaudio.output.bufferCount);
    if (def->output.bufferLength)
        virCommandAddEnvFormat(cmd, "QEMU_COREAUDIO_BUFFER_SIZE=%u",
                               def->output.bufferLength);
}

static void
qemuBuildAudioOSSEnv(virCommand *cmd,
                     const char *prefix,
                     const char *prefix2,
                     virDomainAudioIOOSS *def)
{
    if (def->dev)
        virCommandAddEnvFormat(cmd, "%sDEV=%s",
                               prefix, def->dev);
    if (def->tryPoll)
        virCommandAddEnvFormat(cmd, "%sTRY_POLL=%s", prefix2,
                               virTristateSwitchTypeToString(def->tryPoll));
}

static void
qemuBuildAudioPulseAudioEnv(virCommand *cmd,
                            virDomainAudioDef *def)
{
    if (def->backend.pulseaudio.input.name)
        virCommandAddEnvPair(cmd, "QEMU_PA_SOURCE",
                             def->backend.pulseaudio.input.name);
    if (def->backend.pulseaudio.output.name)
        virCommandAddEnvPair(cmd, "QEMU_PA_SINK",
                             def->backend.pulseaudio.output.name);

    if (def->input.bufferLength)
        virCommandAddEnvFormat(cmd, "QEMU_PA_SAMPLES=%u",
                               def->input.bufferLength);

    if (def->backend.pulseaudio.serverName)
        virCommandAddEnvPair(cmd, "QEMU_PA_SERVER=%s",
                             def->backend.pulseaudio.serverName);
}


static int
qemuBuildAudioCommandLineEnv(virCommand *cmd,
                             virDomainDef *def)
{
    virDomainAudioDef *audio;
    if (def->naudios != 1)
        return 0;

    audio = def->audios[0];
    virCommandAddEnvPair(cmd, "QEMU_AUDIO_DRV",
                         qemuAudioDriverTypeToString(audio->type));

    if (audio->timerPeriod)
        virCommandAddEnvFormat(cmd, "QEMU_AUDIO_TIMER_PERIOD=%u",
                               audio->timerPeriod);

    qemuBuildAudioCommonEnv(cmd, "QEMU_AUDIO_ADC_", &audio->input);
    qemuBuildAudioCommonEnv(cmd, "QEMU_AUDIO_DAC_", &audio->output);

    switch (audio->type) {
    case VIR_DOMAIN_AUDIO_TYPE_NONE:
        break;

    case VIR_DOMAIN_AUDIO_TYPE_ALSA:
        qemuBuildAudioALSAEnv(cmd, "QEMU_AUDIO_ADC_", &audio->backend.alsa.input);
        qemuBuildAudioALSAEnv(cmd, "QEMU_AUDIO_DAC_", &audio->backend.alsa.output);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_COREAUDIO:
        qemuBuildAudioCoreAudioEnv(cmd, audio);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_JACK:
        break;

    case VIR_DOMAIN_AUDIO_TYPE_OSS:
        qemuBuildAudioOSSEnv(cmd, "QEMU_OSS_ADC_", "QEMU_AUDIO_ADC_",
                             &audio->backend.oss.input);
        qemuBuildAudioOSSEnv(cmd, "QEMU_OSS_DAC_", "QEMU_AUDIO_DAC_",
                             &audio->backend.oss.output);

        if (audio->backend.oss.input.bufferCount)
            virCommandAddEnvFormat(cmd, "QEMU_OSS_NFRAGS=%u",
                                   audio->backend.oss.input.bufferCount);

        if (audio->backend.oss.tryMMap)
            virCommandAddEnvFormat(cmd, "QEMU_OSS_MMAP=%s",
                                   virTristateSwitchTypeToString(audio->backend.oss.tryMMap));
        if (audio->backend.oss.exclusive)
            virCommandAddEnvFormat(cmd, "QEMU_OSS_EXCLUSIVE=%s",
                                   virTristateSwitchTypeToString(audio->backend.oss.exclusive));
        if (audio->backend.oss.dspPolicySet)
            virCommandAddEnvFormat(cmd, "QEMU_OSS_POLICY=%d",
                                   audio->backend.oss.dspPolicy);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_PULSEAUDIO:
        qemuBuildAudioPulseAudioEnv(cmd, audio);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_SDL:
        if (audio->output.bufferLength)
            virCommandAddEnvFormat(cmd, "QEMU_SDL_SAMPLES=%u",
                                   audio->output.bufferLength);

        if (audio->backend.sdl.driver) {
            /*
             * Some SDL audio driver names are different on SDL 1.2
             * vs 2.0.  Given how old SDL 1.2 is, we're not going
             * make any attempt to support it here as it is unlikely
             * to have an real world users. We can assume libvirt
             * driver name strings match SDL 2.0 names.
             */
            virCommandAddEnvPair(cmd, "SDL_AUDIODRIVER",
                                 virDomainAudioSDLDriverTypeToString(
                                     audio->backend.sdl.driver));
        }
        break;

    case VIR_DOMAIN_AUDIO_TYPE_SPICE:
        break;

    case VIR_DOMAIN_AUDIO_TYPE_FILE:
        if (audio->backend.file.path)
            virCommandAddEnvFormat(cmd, "QEMU_WAV_PATH=%s",
                                   audio->backend.file.path);
        break;

    case VIR_DOMAIN_AUDIO_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainAudioType, audio->type);
        return -1;
    }
    return 0;
}

static int
qemuBuildAudioCommandLine(virCommand *cmd,
                          virDomainDef *def,
                          virQEMUCaps *qemuCaps)
{
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_AUDIODEV))
        return qemuBuildAudioCommandLineArgs(cmd, def);
    else
        return qemuBuildAudioCommandLineEnv(cmd, def);
}


static int
qemuBuildGraphicsSDLCommandLine(virQEMUDriverConfig *cfg G_GNUC_UNUSED,
                                virCommand *cmd,
                                virQEMUCaps *qemuCaps G_GNUC_UNUSED,
                                virDomainGraphicsDef *graphics)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;

    if (graphics->data.sdl.xauth)
        virCommandAddEnvPair(cmd, "XAUTHORITY", graphics->data.sdl.xauth);
    if (graphics->data.sdl.display)
        virCommandAddEnvPair(cmd, "DISPLAY", graphics->data.sdl.display);
    if (graphics->data.sdl.fullscreen)
        virCommandAddArg(cmd, "-full-screen");

    virCommandAddArg(cmd, "-display");
    virBufferAddLit(&opt, "sdl");

    if (graphics->data.sdl.gl != VIR_TRISTATE_BOOL_ABSENT)
        virBufferAsprintf(&opt, ",gl=%s",
                          virTristateSwitchTypeToString(graphics->data.sdl.gl));

    virCommandAddArgBuffer(cmd, &opt);

    return 0;
}


static int
qemuBuildGraphicsVNCCommandLine(virQEMUDriverConfig *cfg,
                                const virDomainDef *def,
                                virCommand *cmd,
                                virQEMUCaps *qemuCaps,
                                virDomainGraphicsDef *graphics)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;
    virDomainGraphicsListenDef *glisten = NULL;
    bool escapeAddr;

    if (!(glisten = virDomainGraphicsGetListen(graphics, 0))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing listen element"));
        return -1;
    }

    switch (glisten->type) {
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET:
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_MULTI_SERVERS))
            virBufferAddLit(&opt, "vnc=unix:");
        else
            virBufferAddLit(&opt, "unix:");
        virQEMUBuildBufferEscapeComma(&opt, glisten->socket);
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
        if (!graphics->data.vnc.autoport &&
            (graphics->data.vnc.port < 5900 ||
             graphics->data.vnc.port > 65535)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("vnc port must be in range [5900,65535]"));
            return -1;
        }

        if (glisten->address) {
            escapeAddr = strchr(glisten->address, ':') != NULL;
            if (escapeAddr)
                virBufferAsprintf(&opt, "[%s]", glisten->address);
            else
                virBufferAdd(&opt, glisten->address, -1);
        }
        virBufferAsprintf(&opt, ":%d",
                          graphics->data.vnc.port - 5900);

        if (graphics->data.vnc.websocket)
            virBufferAsprintf(&opt, ",websocket=%d", graphics->data.vnc.websocket);
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE:
        virBufferAddLit(&opt, "none");
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST:
        break;
    }

    if (graphics->data.vnc.sharePolicy) {
        virBufferAsprintf(&opt, ",share=%s",
                          virDomainGraphicsVNCSharePolicyTypeToString(
                              graphics->data.vnc.sharePolicy));
    }

    if (graphics->data.vnc.auth.passwd || cfg->vncPassword) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_OPTS))
            virBufferAddLit(&opt, ",password=on");
        else
            virBufferAddLit(&opt, ",password");
    }

    if (cfg->vncTLS) {
        qemuDomainGraphicsPrivate *gfxPriv = QEMU_DOMAIN_GRAPHICS_PRIVATE(graphics);
        if (gfxPriv->tlsAlias) {
            const char *secretAlias = NULL;

            if (gfxPriv->secinfo) {
                if (qemuBuildObjectSecretCommandLine(cmd,
                                                     gfxPriv->secinfo,
                                                     qemuCaps) < 0)
                    return -1;
                secretAlias = gfxPriv->secinfo->s.aes.alias;
            }

            if (qemuBuildTLSx509CommandLine(cmd,
                                            cfg->vncTLSx509certdir,
                                            true,
                                            cfg->vncTLSx509verify,
                                            secretAlias,
                                            gfxPriv->tlsAlias,
                                            qemuCaps) < 0)
                return -1;

            virBufferAsprintf(&opt, ",tls-creds=%s", gfxPriv->tlsAlias);
        } else {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_OPTS))
                virBufferAddLit(&opt, ",tls=on");
            else
                virBufferAddLit(&opt, ",tls");
            if (cfg->vncTLSx509verify) {
                virBufferAddLit(&opt, ",x509verify=");
                virQEMUBuildBufferEscapeComma(&opt, cfg->vncTLSx509certdir);
            } else {
                virBufferAddLit(&opt, ",x509=");
                virQEMUBuildBufferEscapeComma(&opt, cfg->vncTLSx509certdir);
            }
        }
    }

    if (cfg->vncSASL) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_OPTS))
            virBufferAddLit(&opt, ",sasl=on");
        else
            virBufferAddLit(&opt, ",sasl");

        if (cfg->vncSASLdir)
            virCommandAddEnvPair(cmd, "SASL_CONF_PATH", cfg->vncSASLdir);

        /* TODO: Support ACLs later */
    }

    if (graphics->data.vnc.powerControl != VIR_TRISTATE_BOOL_ABSENT) {
        virBufferAsprintf(&opt, ",power-control=%s",
                          graphics->data.vnc.powerControl == VIR_TRISTATE_BOOL_YES ?
                          "on" : "off");
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_AUDIODEV)) {
        g_autofree char *audioid = qemuGetAudioIDString(def, graphics->data.vnc.audioId);
        if (!audioid)
            return -1;
        virBufferAsprintf(&opt, ",audiodev=%s", audioid);
    }

    virCommandAddArg(cmd, "-vnc");
    virCommandAddArgBuffer(cmd, &opt);
    if (graphics->data.vnc.keymap)
        virCommandAddArgList(cmd, "-k", graphics->data.vnc.keymap, NULL);

    return 0;
}


static int
qemuBuildGraphicsSPICECommandLine(virQEMUDriverConfig *cfg,
                                  virCommand *cmd,
                                  virDomainGraphicsDef *graphics)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;
    virDomainGraphicsListenDef *glisten = NULL;
    int port = graphics->data.spice.port;
    int tlsPort = graphics->data.spice.tlsPort;
    size_t i;
    bool hasSecure = false;
    bool hasInsecure = false;

    if (!(glisten = virDomainGraphicsGetListen(graphics, 0))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing listen element"));
        return -1;
    }

    switch (glisten->type) {
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET:
        virBufferAddLit(&opt, "unix,addr=");
        virQEMUBuildBufferEscapeComma(&opt, glisten->socket);
        virBufferAddLit(&opt, ",");
        hasInsecure = true;
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
        if (port > 0) {
            virBufferAsprintf(&opt, "port=%u,", port);
            hasInsecure = true;
        }

        if (tlsPort > 0) {
            virBufferAsprintf(&opt, "tls-port=%u,", tlsPort);
            hasSecure = true;
        }

        if (port > 0 || tlsPort > 0) {
            if (glisten->address)
                virBufferAsprintf(&opt, "addr=%s,", glisten->address);
        }

        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE:
        /* QEMU requires either port or tls-port to be specified if there is no
         * other argument. Use a dummy port=0. */
        virBufferAddLit(&opt, "port=0,");
        hasInsecure = true;
        break;
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST:
        break;
    }

    if (cfg->spiceSASL) {
        virBufferAddLit(&opt, "sasl=on,");

        if (cfg->spiceSASLdir)
            virCommandAddEnvPair(cmd, "SASL_CONF_PATH",
                                 cfg->spiceSASLdir);

        /* TODO: Support ACLs later */
    }

    if (graphics->data.spice.mousemode) {
        switch (graphics->data.spice.mousemode) {
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_SERVER:
            virBufferAddLit(&opt, "agent-mouse=off,");
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_CLIENT:
            virBufferAddLit(&opt, "agent-mouse=on,");
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_DEFAULT:
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_LAST:
        default:
            virReportEnumRangeError(virDomainGraphicsSpiceMouseMode,
                                    graphics->data.spice.mousemode);
            return -1;
        }
    }

    /* In the password case we set it via monitor command, to avoid
     * making it visible on CLI, so there's no use of password=XXX
     * in this bit of the code */
    if (!graphics->data.spice.auth.passwd &&
        !cfg->spicePassword)
        virBufferAddLit(&opt, "disable-ticketing=on,");

    if (hasSecure) {
        virBufferAddLit(&opt, "x509-dir=");
        virQEMUBuildBufferEscapeComma(&opt, cfg->spiceTLSx509certdir);
        virBufferAddLit(&opt, ",");
    }

    switch (graphics->data.spice.defaultMode) {
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
        if (!hasSecure) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spice defaultMode secure requested in XML "
                             "configuration, but TLS connection is not "
                             "available"));
            return -1;
        }
        virBufferAddLit(&opt, "tls-channel=default,");
        break;
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
        if (!hasInsecure) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spice defaultMode insecure requested in XML "
                             "configuration, but plaintext connection is not "
                             "available"));
            return -1;
        }
        virBufferAddLit(&opt, "plaintext-channel=default,");
        break;
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_LAST:
        /* nothing */
        break;
    }

    for (i = 0; i < VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST; i++) {
        switch (graphics->data.spice.channels[i]) {
        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
            if (!hasSecure) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice secure channels set in XML "
                                 "configuration, but TLS connection is not "
                                 "available"));
                return -1;
            }
            virBufferAsprintf(&opt, "tls-channel=%s,",
                              virDomainGraphicsSpiceChannelNameTypeToString(i));
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
            if (!hasInsecure) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice insecure channels set in XML "
                                 "configuration, but plaintext connection "
                                 "is not available"));
                return -1;
            }
            virBufferAsprintf(&opt, "plaintext-channel=%s,",
                              virDomainGraphicsSpiceChannelNameTypeToString(i));
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
            break;
        }
    }

    if (graphics->data.spice.image)
        virBufferAsprintf(&opt, "image-compression=%s,",
                          virDomainGraphicsSpiceImageCompressionTypeToString(graphics->data.spice.image));
    if (graphics->data.spice.jpeg)
        virBufferAsprintf(&opt, "jpeg-wan-compression=%s,",
                          virDomainGraphicsSpiceJpegCompressionTypeToString(graphics->data.spice.jpeg));
    if (graphics->data.spice.zlib)
        virBufferAsprintf(&opt, "zlib-glz-wan-compression=%s,",
                          virDomainGraphicsSpiceZlibCompressionTypeToString(graphics->data.spice.zlib));
    if (graphics->data.spice.playback)
        virBufferAsprintf(&opt, "playback-compression=%s,",
                          virTristateSwitchTypeToString(graphics->data.spice.playback));
    if (graphics->data.spice.streaming)
        virBufferAsprintf(&opt, "streaming-video=%s,",
                          virDomainGraphicsSpiceStreamingModeTypeToString(graphics->data.spice.streaming));
    if (graphics->data.spice.copypaste == VIR_TRISTATE_BOOL_NO)
        virBufferAddLit(&opt, "disable-copy-paste=on,");

    if (graphics->data.spice.filetransfer == VIR_TRISTATE_BOOL_NO)
        virBufferAddLit(&opt, "disable-agent-file-xfer=on,");

    if (graphics->data.spice.gl == VIR_TRISTATE_BOOL_YES) {
        /* spice.gl is a TristateBool, but qemu expects on/off: use
         * TristateSwitch helper */
        virBufferAsprintf(&opt, "gl=%s,",
                          virTristateSwitchTypeToString(graphics->data.spice.gl));

        if (graphics->data.spice.rendernode) {
            virBufferAddLit(&opt, "rendernode=");
            virQEMUBuildBufferEscapeComma(&opt, graphics->data.spice.rendernode);
            virBufferAddLit(&opt, ",");
        }
    }

    /* Turn on seamless migration unconditionally. If migration destination
     * doesn't support it, it fallbacks to previous migration algorithm silently. */
    virBufferAddLit(&opt, "seamless-migration=on,");

    virBufferTrim(&opt, ",");

    virCommandAddArg(cmd, "-spice");
    virCommandAddArgBuffer(cmd, &opt);
    if (graphics->data.spice.keymap)
        virCommandAddArgList(cmd, "-k",
                             graphics->data.spice.keymap, NULL);

    return 0;
}


static int
qemuBuildGraphicsEGLHeadlessCommandLine(virQEMUDriverConfig *cfg G_GNUC_UNUSED,
                                        virCommand *cmd,
                                        virDomainGraphicsDef *graphics)
{
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&opt, "egl-headless");

    if (graphics->data.egl_headless.rendernode) {
        virBufferAddLit(&opt, ",rendernode=");
        virQEMUBuildBufferEscapeComma(&opt,
                                      graphics->data.egl_headless.rendernode);
    }

    virCommandAddArg(cmd, "-display");
    virCommandAddArgBuffer(cmd, &opt);

    return 0;
}


static int
qemuBuildGraphicsCommandLine(virQEMUDriverConfig *cfg,
                             virCommand *cmd,
                             virDomainDef *def,
                             virQEMUCaps *qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ngraphics; i++) {
        virDomainGraphicsDef *graphics = def->graphics[i];

        switch (graphics->type) {
        case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
            if (qemuBuildGraphicsSDLCommandLine(cfg, cmd,
                                                qemuCaps, graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
            if (qemuBuildGraphicsVNCCommandLine(cfg, def, cmd,
                                                qemuCaps, graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
            if (qemuBuildGraphicsSPICECommandLine(cfg, cmd,
                                                  graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
            if (qemuBuildGraphicsEGLHeadlessCommandLine(cfg, cmd,
                                                        graphics) < 0)
                return -1;

            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
            return -1;
        case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
        default:
            virReportEnumRangeError(virDomainGraphicsType, graphics->type);
            return -1;
        }
    }

    return 0;
}

static int
qemuInterfaceVhostuserConnect(virQEMUDriver *driver,
                              virLogManager *logManager,
                              virSecurityManager *secManager,
                              virCommand *cmd,
                              virDomainDef *def,
                              virDomainNetDef *net,
                              virQEMUCaps *qemuCaps,
                              char **chardev)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);

    switch ((virDomainChrType)net->data.vhostuser->type) {
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (!(*chardev = qemuBuildChrChardevStr(logManager, secManager,
                                                cmd, cfg, def,
                                                net->data.vhostuser,
                                                net->info.alias, qemuCaps, 0)))
            return -1;
        break;

    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_UDP:
    case VIR_DOMAIN_CHR_TYPE_TCP:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
    case VIR_DOMAIN_CHR_TYPE_NMDM:
    case VIR_DOMAIN_CHR_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("vhost-user type '%s' not supported"),
                       virDomainChrTypeToString(net->data.vhostuser->type));
        return -1;
    }

    return 0;
}

static int
qemuBuildInterfaceCommandLine(virQEMUDriver *driver,
                              virDomainObj *vm,
                              virLogManager *logManager,
                              virSecurityManager *secManager,
                              virCommand *cmd,
                              virDomainNetDef *net,
                              virQEMUCaps *qemuCaps,
                              unsigned int bootindex,
                              virNetDevVPortProfileOp vmop,
                              bool standalone,
                              size_t *nnicindexes,
                              int **nicindexes,
                              unsigned int flags)
{
    virDomainDef *def = vm->def;
    int ret = -1;
    g_autofree char *nic = NULL;
    g_autofree char *host = NULL;
    g_autofree char *chardev = NULL;
    int *tapfd = NULL;
    size_t tapfdSize = 0;
    int *vhostfd = NULL;
    size_t vhostfdSize = 0;
    char **tapfdName = NULL;
    char **vhostfdName = NULL;
    g_autofree char *slirpfdName = NULL;
    g_autofree char *vdpafdName = NULL;
    int vdpafd = -1;
    virDomainNetType actualType = virDomainNetGetActualType(net);
    const virNetDevBandwidth *actualBandwidth;
    bool requireNicdev = false;
    qemuSlirp *slirp;
    size_t i;
    g_autoptr(virJSONValue) hostnetprops = NULL;


    if (!bootindex)
        bootindex = net->info.bootIndex;

    if (qemuDomainValidateActualNetDef(net, qemuCaps) < 0)
        return -1;

    switch (actualType) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        tapfd = g_new0(int, tapfdSize);
        tapfdName = g_new0(char *, tapfdSize);

        memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

        if (qemuInterfaceBridgeConnect(def, driver, net,
                                       tapfd, &tapfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        tapfd = g_new0(int, tapfdSize);
        tapfdName = g_new0(char *, tapfdSize);

        memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

        if (qemuInterfaceDirectConnect(def, driver, net,
                                       tapfd, tapfdSize, vmop) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        tapfd = g_new0(int, tapfdSize);
        tapfdName = g_new0(char *, tapfdSize);

        memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

        if (qemuInterfaceEthernetConnect(def, driver, net,
                                         tapfd, tapfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        /* NET_TYPE_HOSTDEV devices are really hostdev devices, so
         * their commandlines are constructed with other hostdevs.
         */
        ret = 0;
        goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        requireNicdev = true;

        if (qemuInterfaceVhostuserConnect(driver, logManager, secManager,
                                          cmd, def, net, qemuCaps, &chardev) < 0)
            goto cleanup;

        if (virNetDevOpenvswitchGetVhostuserIfname(net->data.vhostuser->data.nix.path,
                                                   net->data.vhostuser->data.nix.listen,
                                                   &net->ifname) < 0)
            goto cleanup;

        break;

    case VIR_DOMAIN_NET_TYPE_VDPA:
        if ((vdpafd = qemuInterfaceVDPAConnect(net)) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_LAST:
        /* nada */
        break;
    }

    /* For types whose implementations use a netdev on the host, add
     * an entry to nicindexes for passing on to systemd.
    */
    switch ((virDomainNetType)actualType) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    {
        int nicindex;

        /* network and bridge use a tap device, and direct uses a
         * macvtap device
         */
        if (driver->privileged && nicindexes && nnicindexes &&
            net->ifname) {
            if (virNetDevGetIndex(net->ifname, &nicindex) < 0)
                goto cleanup;

            VIR_APPEND_ELEMENT(*nicindexes, *nnicindexes, nicindex);
        }
        break;
    }

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_VDPA:
    case VIR_DOMAIN_NET_TYPE_LAST:
       /* These types don't use a network device on the host, but
        * instead use some other type of connection to the emulated
        * device in the qemu process.
        *
        * (Note that hostdev can't be considered as "using a network
        * device", because by the time it is being used, it has been
        * detached from the hostside network driver so it doesn't show
        * up in the list of interfaces on the host - it's just some
        * PCI device.)
        */
       break;
    }

    qemuDomainInterfaceSetDefaultQDisc(driver, net);

    /* Set bandwidth or warn if requested and not supported. */
    actualBandwidth = virDomainNetGetActualBandwidth(net);
    if (actualBandwidth) {
        if (virNetDevSupportsBandwidth(actualType)) {
            if (virDomainNetDefIsOvsport(net)) {
                if (virNetDevOpenvswitchInterfaceSetQos(net->ifname, actualBandwidth,
                                                        def->uuid,
                                                        !virDomainNetTypeSharesHostView(net)) < 0)
                    goto cleanup;
            } else if (virNetDevBandwidthSet(net->ifname, actualBandwidth, false,
                                             !virDomainNetTypeSharesHostView(net)) < 0) {
                goto cleanup;
            }
        } else {
            VIR_WARN("setting bandwidth on interfaces of "
                     "type '%s' is not implemented yet",
                     virDomainNetTypeToString(actualType));
        }
    }

    if (net->mtu && net->managed_tap != VIR_TRISTATE_BOOL_NO &&
        virNetDevSetMTU(net->ifname, net->mtu) < 0)
        goto cleanup;

    if ((actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
         actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
         actualType == VIR_DOMAIN_NET_TYPE_ETHERNET ||
         actualType == VIR_DOMAIN_NET_TYPE_DIRECT) &&
        !standalone) {
        /* Attempt to use vhost-net mode for these types of
           network device */
        vhostfdSize = net->driver.virtio.queues;
        if (!vhostfdSize)
            vhostfdSize = 1;

        vhostfd = g_new0(int, vhostfdSize);
        vhostfdName = g_new0(char *, vhostfdSize);

        memset(vhostfd, -1, vhostfdSize * sizeof(vhostfd[0]));

        if (qemuInterfaceOpenVhostNet(def, net, vhostfd, &vhostfdSize) < 0)
            goto cleanup;
    }

    slirp = QEMU_DOMAIN_NETWORK_PRIVATE(net)->slirp;
    if (slirp && !standalone) {
        int slirpfd = qemuSlirpGetFD(slirp);
        virCommandPassFD(cmd, slirpfd,
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        slirpfdName = g_strdup_printf("%d", slirpfd);
    }


    for (i = 0; i < tapfdSize; i++) {
        if (qemuSecuritySetTapFDLabel(driver->securityManager,
                                      def, tapfd[i]) < 0)
            goto cleanup;
        tapfdName[i] = g_strdup_printf("%d", tapfd[i]);
        virCommandPassFD(cmd, tapfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        tapfd[i] = -1;
    }

    for (i = 0; i < vhostfdSize; i++) {
        vhostfdName[i] = g_strdup_printf("%d", vhostfd[i]);
        virCommandPassFD(cmd, vhostfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        vhostfd[i] = -1;
    }

    if (vdpafd > 0) {
        g_autofree char *fdset = NULL;
        g_autofree char *addfdarg = NULL;
        size_t idx;

        virCommandPassFDIndex(cmd, vdpafd, VIR_COMMAND_PASS_FD_CLOSE_PARENT, &idx);
        fdset = qemuBuildFDSet(vdpafd, idx);
        vdpafdName = g_strdup_printf("/dev/fdset/%zu", idx);
        /* set opaque to the devicepath so that we can look up the fdset later
         * if necessary */
        addfdarg = g_strdup_printf("%s,opaque=%s", fdset,
                                   net->data.vdpa.devicepath);
        virCommandAddArgList(cmd, "-add-fd", addfdarg, NULL);
        vdpafd = -1;
    }

    if (chardev)
        virCommandAddArgList(cmd, "-chardev", chardev, NULL);

    if (!(hostnetprops = qemuBuildHostNetStr(net,
                                             tapfdName, tapfdSize,
                                             vhostfdName, vhostfdSize,
                                             slirpfdName, vdpafdName)))
        goto cleanup;

    if (!(host = virQEMUBuildNetdevCommandlineFromJSON(hostnetprops,
                                                       (flags & QEMU_BUILD_COMMANDLINE_VALIDATE_KEEP_JSON))))
        goto cleanup;

    virCommandAddArgList(cmd, "-netdev", host, NULL);

    /* Possible combinations:
     *
     *   Old way: -netdev type=tap,id=netdev1 \
     *              -net nic,model=e1000,netdev=netdev1
     *   New way: -netdev type=tap,id=netdev1 -device e1000,id=netdev1
     */
    if (qemuDomainSupportsNicdev(def, net)) {
        if (qemuCommandAddExtDevice(cmd, &net->info) < 0)
            goto cleanup;

        if (!(nic = qemuBuildNicDevStr(def, net, bootindex,
                                       net->driver.virtio.queues, qemuCaps)))
            goto cleanup;
        virCommandAddArgList(cmd, "-device", nic, NULL);
    } else if (!requireNicdev) {
        if (qemuCommandAddExtDevice(cmd, &net->info) < 0)
            goto cleanup;

        if (!(nic = qemuBuildLegacyNicStr(net)))
            goto cleanup;
        virCommandAddArgList(cmd, "-net", nic, NULL);
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Nicdev support unavailable"));
        goto cleanup;
    }

    ret = 0;
 cleanup:
    if (ret < 0) {
        virErrorPtr saved_err;

        virErrorPreserveLast(&saved_err);
        virDomainConfNWFilterTeardown(net);
        virErrorRestore(&saved_err);
    }
    for (i = 0; vhostfd && i < vhostfdSize; i++) {
        if (ret < 0)
            VIR_FORCE_CLOSE(vhostfd[i]);
        if (vhostfdName)
            VIR_FREE(vhostfdName[i]);
    }
    VIR_FREE(vhostfdName);
    for (i = 0; tapfd && i < tapfdSize; i++) {
        if (ret < 0)
            VIR_FORCE_CLOSE(tapfd[i]);
        if (tapfdName)
            VIR_FREE(tapfdName[i]);
    }
    VIR_FREE(tapfdName);
    VIR_FREE(vhostfd);
    VIR_FREE(tapfd);
    VIR_FORCE_CLOSE(vdpafd);
    return ret;
}


/* NOTE: Not using const virDomainDef here since eventually a call is made
 *       into qemuSecuritySetTapFDLabel which calls it's driver
 *       API domainSetSecurityTapFDLabel that doesn't use the const format.
 */
static int
qemuBuildNetCommandLine(virQEMUDriver *driver,
                        virDomainObj *vm,
                        virLogManager *logManager,
                        virSecurityManager *secManager,
                        virCommand *cmd,
                        virQEMUCaps *qemuCaps,
                        virNetDevVPortProfileOp vmop,
                        bool standalone,
                        size_t *nnicindexes,
                        int **nicindexes,
                        unsigned int *bootHostdevNet,
                        unsigned int flags)
{
    size_t i;
    int last_good_net = -1;
    virErrorPtr originalError = NULL;
    virDomainDef *def = vm->def;

    if (def->nnets) {
        unsigned int bootNet = 0;

        /* convert <boot dev='network'/> to bootindex since we didn't emit -boot n */
        for (i = 0; i < def->os.nBootDevs; i++) {
            if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_NET) {
                bootNet = i + 1;
                break;
            }
        }

        for (i = 0; i < def->nnets; i++) {
            virDomainNetDef *net = def->nets[i];

            if (qemuBuildInterfaceCommandLine(driver, vm, logManager, secManager, cmd, net,
                                              qemuCaps, bootNet, vmop,
                                              standalone, nnicindexes,
                                              nicindexes, flags) < 0)
                goto error;

            last_good_net = i;
            /* if this interface is a type='hostdev' interface and we
             * haven't yet added a "bootindex" parameter to an
             * emulated network device, save the bootindex - hostdev
             * interface commandlines will be built later on when we
             * cycle through all the hostdevs, and we'll use it then.
             */
            if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
                *bootHostdevNet == 0) {
                *bootHostdevNet = bootNet;
            }
            bootNet = 0;
        }
    }
    return 0;

 error:
    /* free up any resources in the network driver
     * but don't overwrite the original error */
    virErrorPreserveLast(&originalError);
    for (i = 0; last_good_net != -1 && i <= last_good_net; i++)
        virDomainConfNWFilterTeardown(def->nets[i]);
    virErrorRestore(&originalError);
    return -1;
}


static const char *
qemuBuildSmartcardFindCCIDController(const virDomainDef *def,
                                     const virDomainSmartcardDef *smartcard)
{
    size_t i;

    /* Should never happen. But doesn't hurt to check. */
    if (smartcard->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID)
        return NULL;

    for (i = 0; i < def->ncontrollers; i++) {
        const virDomainControllerDef *tmp = def->controllers[i];

        if (tmp->type != VIR_DOMAIN_CONTROLLER_TYPE_CCID)
            continue;

        if (tmp->idx != smartcard->info.addr.ccid.controller)
            continue;

        return tmp->info.alias;
    }

    return NULL;
}


static int
qemuBuildSmartcardCommandLine(virLogManager *logManager,
                              virSecurityManager *secManager,
                              virCommand *cmd,
                              virQEMUDriverConfig *cfg,
                              const virDomainDef *def,
                              virQEMUCaps *qemuCaps,
                              bool chardevStdioLogd)
{
    size_t i;
    virDomainSmartcardDef *smartcard;
    g_autofree char *devstr = NULL;
    g_auto(virBuffer) opt = VIR_BUFFER_INITIALIZER;
    const char *database;
    const char *contAlias = NULL;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (!def->nsmartcards)
        return 0;

    smartcard = def->smartcards[0];

    /* -device usb-ccid was already emitted along with other
     * controllers.  For now, qemu handles only one smartcard.  */
    if (def->nsmartcards > 1 ||
        smartcard->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID ||
        smartcard->info.addr.ccid.controller != 0 ||
        smartcard->info.addr.ccid.slot != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("this QEMU binary lacks multiple smartcard "
                         "support"));
        return -1;
    }

    switch (smartcard->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        virBufferAddLit(&opt, "ccid-card-emulated,backend=nss-emulated");
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        virBufferAddLit(&opt, "ccid-card-emulated,backend=certificates");
        for (i = 0; i < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; i++) {
            virBufferAsprintf(&opt, ",cert%zu=", i + 1);
            virQEMUBuildBufferEscapeComma(&opt, smartcard->data.cert.file[i]);
        }
        if (smartcard->data.cert.database) {
            database = smartcard->data.cert.database;
        } else {
            database = VIR_DOMAIN_SMARTCARD_DEFAULT_DATABASE;
        }
        virBufferAddLit(&opt, ",db=");
        virQEMUBuildBufferEscapeComma(&opt, database);
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              smartcard->data.passthru,
                                              smartcard->info.alias,
                                              qemuCaps, cdevflags))) {
            return -1;
        }
        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);

        virBufferAsprintf(&opt, "ccid-card-passthru,chardev=char%s",
                          smartcard->info.alias);
        break;

    default:
        virReportEnumRangeError(virDomainSmartcardType, smartcard->type);
        return -1;
    }

    if (!(contAlias = qemuBuildSmartcardFindCCIDController(def,
                                                           smartcard))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to find controller for %s"),
                       smartcard->info.alias);
        return -1;
    }

    virCommandAddArg(cmd, "-device");
    virBufferAsprintf(&opt, ",id=%s,bus=%s.0", smartcard->info.alias, contAlias);
    virCommandAddArgBuffer(cmd, &opt);

    return 0;
}


static char *
qemuBuildShmemDevLegacyStr(virDomainDef *def,
                           virDomainShmemDef *shmem,
                           virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "ivshmem");
    virBufferAsprintf(&buf, ",id=%s", shmem->info.alias);

    if (shmem->size)
        virBufferAsprintf(&buf, ",size=%llum", shmem->size >> 20);

    if (!shmem->server.enabled) {
        virBufferAsprintf(&buf, ",shm=%s", shmem->name);
    } else {
        virBufferAsprintf(&buf, ",chardev=char%s", shmem->info.alias);
        if (shmem->msi.enabled) {
            virBufferAddLit(&buf, ",msi=on");
            if (shmem->msi.vectors)
                virBufferAsprintf(&buf, ",vectors=%u", shmem->msi.vectors);
            if (shmem->msi.ioeventfd)
                virBufferAsprintf(&buf, ",ioeventfd=%s",
                                  virTristateSwitchTypeToString(shmem->msi.ioeventfd));
        }
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &shmem->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}

char *
qemuBuildShmemDevStr(virDomainDef *def,
                     virDomainShmemDef *shmem,
                     virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virBufferAdd(&buf, virDomainShmemModelTypeToString(shmem->model), -1);
    virBufferAsprintf(&buf, ",id=%s", shmem->info.alias);

    if (shmem->server.enabled) {
        virBufferAsprintf(&buf, ",chardev=char%s", shmem->info.alias);
    } else {
        virBufferAsprintf(&buf, ",memdev=shmmem-%s", shmem->info.alias);

        switch (shmem->role) {
        case VIR_DOMAIN_SHMEM_ROLE_MASTER:
            virBufferAddLit(&buf, ",master=on");
            break;
        case VIR_DOMAIN_SHMEM_ROLE_PEER:
            virBufferAddLit(&buf, ",master=off");
            break;
        case VIR_DOMAIN_SHMEM_ROLE_DEFAULT:
        case VIR_DOMAIN_SHMEM_ROLE_LAST:
            break;
        }
    }

    if (shmem->msi.vectors)
        virBufferAsprintf(&buf, ",vectors=%u", shmem->msi.vectors);
    if (shmem->msi.ioeventfd) {
        virBufferAsprintf(&buf, ",ioeventfd=%s",
                          virTristateSwitchTypeToString(shmem->msi.ioeventfd));
    }

    if (qemuBuildDeviceAddressStr(&buf, def, &shmem->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


virJSONValue *
qemuBuildShmemBackendMemProps(virDomainShmemDef *shmem)
{
    g_autofree char *mem_alias = NULL;
    g_autofree char *mem_path = NULL;
    virJSONValue *ret = NULL;

    mem_path = g_strdup_printf("/dev/shm/%s", shmem->name);

    mem_alias = g_strdup_printf("shmmem-%s", shmem->info.alias);

    qemuMonitorCreateObjectProps(&ret, "memory-backend-file", mem_alias,
                                 "s:mem-path", mem_path,
                                 "U:size", shmem->size,
                                 "b:share", true,
                                 NULL);

    return ret;
}


static int
qemuBuildShmemCommandLine(virLogManager *logManager,
                          virSecurityManager *secManager,
                          virCommand *cmd,
                          virQEMUDriverConfig *cfg,
                          virDomainDef *def,
                          virDomainShmemDef *shmem,
                          virQEMUCaps *qemuCaps,
                          bool chardevStdioLogd)
{
    g_autoptr(virJSONValue) memProps = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *devstr = NULL;
    g_autofree char *chardev = NULL;
    int rc;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (shmem->size) {
        /*
         * Thanks to our parsing code, we have a guarantee that the
         * size is power of two and is at least a mebibyte in size.
         * But because it may change in the future, the checks are
         * doubled in here.
         */
        if (shmem->size & (shmem->size - 1)) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("shmem size must be a power of two"));
            return -1;
        }
        if (shmem->size < 1024 * 1024) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("shmem size must be at least 1 MiB (1024 KiB)"));
            return -1;
        }
    }

    if (shmem->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only 'pci' addresses are supported for the "
                         "shared memory device"));
        return -1;
    }

    switch (shmem->model) {
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM:
        devstr = qemuBuildShmemDevLegacyStr(def, shmem, qemuCaps);
        break;

    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_PLAIN:
        if (!(memProps = qemuBuildShmemBackendMemProps(shmem)))
            return -1;

        rc = qemuBuildObjectCommandlineFromJSON(&buf, memProps, qemuCaps);

        if (rc < 0)
            return -1;

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);

        G_GNUC_FALLTHROUGH;
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL:
        devstr = qemuBuildShmemDevStr(def, shmem, qemuCaps);
        break;

    case VIR_DOMAIN_SHMEM_MODEL_LAST:
        break;
    }

    if (!devstr)
        return -1;

    if (qemuCommandAddExtDevice(cmd, &shmem->info) < 0)
        return -1;

    virCommandAddArgList(cmd, "-device", devstr, NULL);

    if (shmem->server.enabled) {
        chardev = qemuBuildChrChardevStr(logManager, secManager,
                                        cmd, cfg, def,
                                        &shmem->server.chr,
                                        shmem->info.alias, qemuCaps,
                                        cdevflags);
        if (!chardev)
            return -1;

        virCommandAddArgList(cmd, "-chardev", chardev, NULL);
    }

    return 0;
}


static virQEMUCapsFlags
qemuChrSerialTargetModelToCaps(virDomainChrSerialTargetModel targetModel)
{
    switch (targetModel) {
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL:
        return QEMU_CAPS_DEVICE_ISA_SERIAL;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL:
        return QEMU_CAPS_DEVICE_USB_SERIAL;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL:
        return QEMU_CAPS_DEVICE_PCI_SERIAL;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY:
        return QEMU_CAPS_DEVICE_SPAPR_VTY;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE:
        return QEMU_CAPS_DEVICE_SCLPCONSOLE;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE:
        return QEMU_CAPS_DEVICE_SCLPLMCONSOLE;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011:
        return QEMU_CAPS_DEVICE_PL011;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_16550A:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_LAST:
        break;
    }

    return 0;
}


static int
qemuBuildChrDeviceCommandLine(virCommand *cmd,
                              const virDomainDef *def,
                              virDomainChrDef *chr,
                              virQEMUCaps *qemuCaps)
{
    g_autofree char *devstr = NULL;

    if (qemuBuildChrDeviceStr(&devstr, def, chr, qemuCaps) < 0)
        return -1;

    virCommandAddArgList(cmd, "-device", devstr, NULL);
    return 0;
}


static bool
qemuChrIsPlatformDevice(const virDomainDef *def,
                        virDomainChrDef *chr)
{
    if (def->os.arch == VIR_ARCH_ARMV6L ||
        def->os.arch == VIR_ARCH_ARMV7L ||
        def->os.arch == VIR_ARCH_AARCH64) {

        /* pl011 (used on mach-virt) is a platform device */
        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
            chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM &&
            chr->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011) {
            return true;
        }
    }

    if (ARCH_IS_RISCV(def->os.arch)) {

        /* 16550a (used by riscv/virt guests) is a platform device */
        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
            chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM &&
            chr->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_16550A) {
            return true;
        }
    }

    /* If we got all the way here and we're still stuck with the default
     * target type for a serial device, it means we have no clue what kind of
     * device we're talking about and we must treat it as a platform device. */
    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
        chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE) {
        return true;
    }

    return false;
}


static int
qemuBuildSerialCommandLine(virLogManager *logManager,
                           virSecurityManager *secManager,
                           virCommand *cmd,
                           virQEMUDriverConfig *cfg,
                           const virDomainDef *def,
                           virQEMUCaps *qemuCaps,
                           bool chardevStdioLogd)
{
    size_t i;
    bool havespice = false;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    if (def->nserials) {
        for (i = 0; i < def->ngraphics && !havespice; i++) {
            if (def->graphics[i]->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE)
                havespice = true;
        }
    }

    for (i = 0; i < def->nserials; i++) {
        virDomainChrDef *serial = def->serials[i];
        g_autofree char *devstr = NULL;

        if (serial->source->type == VIR_DOMAIN_CHR_TYPE_SPICEPORT && !havespice)
            continue;

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              serial->source,
                                              serial->info.alias,
                                              qemuCaps, cdevflags)))
            return -1;
        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);

        /* If the device is not a platform device, build the devstr */
        if (!qemuChrIsPlatformDevice(def, serial)) {
            if (qemuBuildChrDeviceCommandLine(cmd, def, serial, qemuCaps) < 0)
                return -1;
        } else {
            virQEMUCapsFlags caps;

            caps = qemuChrSerialTargetModelToCaps(serial->targetModel);

            if (caps && !virQEMUCapsGet(qemuCaps, caps)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("'%s' is not supported in this QEMU binary"),
                               virDomainChrSerialTargetModelTypeToString(serial->targetModel));
                return -1;
            }

            virCommandAddArg(cmd, "-serial");
            virCommandAddArgFormat(cmd, "chardev:char%s", serial->info.alias);
        }
    }

    return 0;
}


static int
qemuBuildParallelsCommandLine(virLogManager *logManager,
                              virSecurityManager *secManager,
                              virCommand *cmd,
                              virQEMUDriverConfig *cfg,
                              const virDomainDef *def,
                              virQEMUCaps *qemuCaps,
                              bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    for (i = 0; i < def->nparallels; i++) {
        virDomainChrDef *parallel = def->parallels[i];
        g_autofree char *devstr = NULL;

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              parallel->source,
                                              parallel->info.alias,
                                              qemuCaps, cdevflags)))
            return -1;
        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);

        if (qemuBuildChrDeviceCommandLine(cmd, def, parallel,
                                          qemuCaps) < 0)
            return -1;
    }

    return 0;
}


static int
qemuBuildChannelsCommandLine(virLogManager *logManager,
                             virSecurityManager *secManager,
                             virCommand *cmd,
                             virQEMUDriverConfig *cfg,
                             const virDomainDef *def,
                             virQEMUCaps *qemuCaps,
                             bool chardevStdioLogd,
                             unsigned int flags)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    for (i = 0; i < def->nchannels; i++) {
        virDomainChrDef *channel = def->channels[i];
        g_autofree char *chardevstr = NULL;
        g_autoptr(virJSONValue) netdevprops = NULL;
        g_autofree char *netdevstr = NULL;

        if (!(chardevstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  channel->source,
                                                  channel->info.alias,
                                                  qemuCaps, cdevflags)))
            return -1;

        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, chardevstr);

        switch ((virDomainChrChannelTargetType) channel->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            if (!(netdevprops = qemuBuildChannelGuestfwdNetdevProps(channel)))
                return -1;

            if (!(netdevstr = virQEMUBuildNetdevCommandlineFromJSON(netdevprops,
                                                                    (flags & QEMU_BUILD_COMMANDLINE_VALIDATE_KEEP_JSON))))
                return -1;

            virCommandAddArgList(cmd, "-netdev", netdevstr, NULL);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            if (qemuBuildChrDeviceCommandLine(cmd, def, channel, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_XEN:
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_NONE:
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_LAST:
            return -1;
        }
    }

    return 0;
}


static int
qemuBuildConsoleCommandLine(virLogManager *logManager,
                            virSecurityManager *secManager,
                            virCommand *cmd,
                            virQEMUDriverConfig *cfg,
                            const virDomainDef *def,
                            virQEMUCaps *qemuCaps,
                            bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    /* Explicit console devices */
    for (i = 0; i < def->nconsoles; i++) {
        virDomainChrDef *console = def->consoles[i];
        char *devstr;

        switch (console->targetType) {
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  console->source,
                                                  console->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  console->source,
                                                  console->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO:
            if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                                  cmd, cfg, def,
                                                  console->source,
                                                  console->info.alias,
                                                  qemuCaps, cdevflags)))
                return -1;
            virCommandAddArg(cmd, "-chardev");
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                return -1;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL:
            break;

        default:
            return -1;
        }
    }

    return 0;
}


char *
qemuBuildRedirdevDevStr(const virDomainDef *def,
                        virDomainRedirdevDef *dev,
                        virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    size_t i;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virDomainRedirFilterDef *redirfilter = def->redirfilter;

    virBufferAsprintf(&buf, "usb-redir,chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);

    if (redirfilter && redirfilter->nusbdevs) {
        virBufferAddLit(&buf, ",filter=");

        for (i = 0; i < redirfilter->nusbdevs; i++) {
            virDomainRedirFilterUSBDevDef *usbdev = redirfilter->usbdevs[i];
            if (usbdev->usbClass >= 0)
                virBufferAsprintf(&buf, "0x%02X:", usbdev->usbClass);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->vendor >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->vendor);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->product >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->product);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->version >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->version);
            else
                virBufferAddLit(&buf, "-1:");

            virBufferAsprintf(&buf, "%u", usbdev->allow);
            if (i < redirfilter->nusbdevs -1)
                virBufferAddLit(&buf, "|");
        }
    }

    if (dev->info.bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%u", dev->info.bootIndex);

    if (qemuBuildDeviceAddressStr(&buf, def, &dev->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildRedirdevCommandLine(virLogManager *logManager,
                             virSecurityManager *secManager,
                             virCommand *cmd,
                             virQEMUDriverConfig *cfg,
                             const virDomainDef *def,
                             virQEMUCaps *qemuCaps,
                             bool chardevStdioLogd)
{
    size_t i;
    unsigned int cdevflags = QEMU_BUILD_CHARDEV_TCP_NOWAIT |
        QEMU_BUILD_CHARDEV_UNIX_FD_PASS;
    if (chardevStdioLogd)
        cdevflags |= QEMU_BUILD_CHARDEV_FILE_LOGD;

    for (i = 0; i < def->nredirdevs; i++) {
        virDomainRedirdevDef *redirdev = def->redirdevs[i];
        char *devstr;

        if (!(devstr = qemuBuildChrChardevStr(logManager, secManager,
                                              cmd, cfg, def,
                                              redirdev->source,
                                              redirdev->info.alias,
                                              qemuCaps, cdevflags))) {
            return -1;
        }

        virCommandAddArg(cmd, "-chardev");
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);

        virCommandAddArg(cmd, "-device");
        if (!(devstr = qemuBuildRedirdevDevStr(def, redirdev, qemuCaps)))
            return -1;
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);
    }

    return 0;
}


static void
qemuBuldDomainLoaderPflashCommandLine(virCommand *cmd,
                                      virDomainLoaderDef *loader,
                                      virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    int unit = 0;

    if (loader->secure == VIR_TRISTATE_BOOL_YES) {
        virCommandAddArgList(cmd,
                             "-global",
                             "driver=cfi.pflash01,property=secure,value=on",
                             NULL);
    }

    /* with blockdev we instantiate the pflash when formatting -machine */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV))
        return;

    virBufferAddLit(&buf, "file=");
    virQEMUBuildBufferEscapeComma(&buf, loader->path);
    virBufferAsprintf(&buf, ",if=pflash,format=raw,unit=%d", unit);
    unit++;

    if (loader->readonly) {
        virBufferAsprintf(&buf, ",readonly=%s",
                          virTristateSwitchTypeToString(loader->readonly));
    }

    virCommandAddArg(cmd, "-drive");
    virCommandAddArgBuffer(cmd, &buf);

    if (loader->nvram) {
        virBufferAddLit(&buf, "file=");
        virQEMUBuildBufferEscapeComma(&buf, loader->nvram);
        virBufferAsprintf(&buf, ",if=pflash,format=raw,unit=%d", unit);

        virCommandAddArg(cmd, "-drive");
        virCommandAddArgBuffer(cmd, &buf);
    }
}


static void
qemuBuildDomainLoaderCommandLine(virCommand *cmd,
                                 virDomainDef *def,
                                 virQEMUCaps *qemuCaps)
{
    virDomainLoaderDef *loader = def->os.loader;

    if (!loader)
        return;

    switch ((virDomainLoader) loader->type) {
    case VIR_DOMAIN_LOADER_TYPE_ROM:
        virCommandAddArg(cmd, "-bios");
        virCommandAddArg(cmd, loader->path);
        break;

    case VIR_DOMAIN_LOADER_TYPE_PFLASH:
        qemuBuldDomainLoaderPflashCommandLine(cmd, loader, qemuCaps);
        break;

    case VIR_DOMAIN_LOADER_TYPE_NONE:
    case VIR_DOMAIN_LOADER_TYPE_LAST:
        /* nada */
        break;
    }
}


static char *
qemuBuildTPMDevStr(const virDomainDef *def,
                   virDomainTPMDef *tpm,
                   virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *model = virDomainTPMModelTypeToString(tpm->model);

    if (tpm->model == VIR_DOMAIN_TPM_MODEL_TIS && def->os.arch == VIR_ARCH_AARCH64)
        model = "tpm-tis-device";

    virBufferAsprintf(&buf, "%s,tpmdev=tpm-%s,id=%s",
                      model, tpm->info.alias, tpm->info.alias);

    if (qemuBuildDeviceAddressStr(&buf, def, &tpm->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


/* this function is exported so that tests can mock the FDs */
int
qemuBuildTPMOpenBackendFDs(const char *tpmdev,
                           const char *cancel_path,
                           int *tpmfd,
                           int *cancelfd)
{
    if ((*tpmfd = open(tpmdev, O_RDWR)) < 0) {
        virReportSystemError(errno, _("Could not open TPM device %s"),
                             tpmdev);
        return -1;
    }

    if ((*cancelfd = open(cancel_path, O_WRONLY)) < 0) {
        virReportSystemError(errno,
                             _("Could not open TPM device's cancel "
                               "path %s"), cancel_path);
        VIR_FORCE_CLOSE(*tpmfd);
        return -1;
    }

    return 0;
}


static char *
qemuBuildTPMBackendStr(virCommand *cmd,
                       virDomainTPMDef *tpm,
                       int *tpmfd,
                       int *cancelfd,
                       char **chardev)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *cancel_path = NULL;
    g_autofree char *devset = NULL;
    g_autofree char *cancelset = NULL;
    const char *tpmdev;

    *tpmfd = -1;
    *cancelfd = -1;

    virBufferAsprintf(&buf, "%s", virDomainTPMBackendTypeToString(tpm->type));
    virBufferAsprintf(&buf, ",id=tpm-%s", tpm->info.alias);

    switch (tpm->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        tpmdev = tpm->data.passthrough.source.data.file.path;
        if (!(cancel_path = virTPMCreateCancelPath(tpmdev)))
            return NULL;

        if (qemuBuildTPMOpenBackendFDs(tpmdev, cancel_path, tpmfd, cancelfd) < 0)
            return NULL;

        virCommandPassFD(cmd, *tpmfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        virCommandPassFD(cmd, *cancelfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);

        if (!(devset = qemuVirCommandGetDevSet(cmd, *tpmfd)) ||
            !(cancelset = qemuVirCommandGetDevSet(cmd, *cancelfd)))
            return NULL;

        virBufferAddLit(&buf, ",path=");
        virQEMUBuildBufferEscapeComma(&buf, devset);

        virBufferAddLit(&buf, ",cancel-path=");
        virQEMUBuildBufferEscapeComma(&buf, cancelset);

        break;
    case VIR_DOMAIN_TPM_TYPE_EMULATOR:
        virBufferAddLit(&buf, ",chardev=chrtpm");

        *chardev = g_strdup_printf("socket,id=chrtpm,path=%s",
                                   tpm->data.emulator.source.data.nix.path);

        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildTPMCommandLine(virCommand *cmd,
                        const virDomainDef *def,
                        virDomainTPMDef *tpm,
                        virQEMUCaps *qemuCaps)
{
    char *optstr;
    g_autofree char *chardev = NULL;
    int tpmfd = -1;
    int cancelfd = -1;
    char *fdset;

    if (!(optstr = qemuBuildTPMBackendStr(cmd, tpm,
                                          &tpmfd, &cancelfd,
                                          &chardev)))
        return -1;

    virCommandAddArgList(cmd, "-tpmdev", optstr, NULL);
    VIR_FREE(optstr);

    if (chardev)
        virCommandAddArgList(cmd, "-chardev", chardev, NULL);

    if (tpmfd >= 0) {
        fdset = qemuVirCommandGetFDSet(cmd, tpmfd);
        if (!fdset)
            return -1;

        virCommandAddArgList(cmd, "-add-fd", fdset, NULL);
        VIR_FREE(fdset);
    }

    if (cancelfd >= 0) {
        fdset = qemuVirCommandGetFDSet(cmd, cancelfd);
        if (!fdset)
            return -1;

        virCommandAddArgList(cmd, "-add-fd", fdset, NULL);
        VIR_FREE(fdset);
    }

    if (!(optstr = qemuBuildTPMDevStr(def, tpm, qemuCaps)))
        return -1;

    virCommandAddArgList(cmd, "-device", optstr, NULL);
    VIR_FREE(optstr);

    return 0;
}


static int
qemuBuildTPMProxyCommandLine(virCommand *cmd,
                             virDomainTPMDef *tpm)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *filePath = NULL;

    filePath = tpm->data.passthrough.source.data.file.path;

    virCommandAddArg(cmd, "-device");
    virBufferAsprintf(&buf, "%s,id=%s,host-path=",
                      virDomainTPMModelTypeToString(tpm->model),
                      tpm->info.alias);
    virQEMUBuildBufferEscapeComma(&buf, filePath);
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


static int
qemuBuildTPMsCommandLine(virCommand *cmd,
                         const virDomainDef *def,
                         virQEMUCaps *qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ntpms; i++) {
        if (def->tpms[i]->model == VIR_DOMAIN_TPM_MODEL_SPAPR_PROXY) {
            if (qemuBuildTPMProxyCommandLine(cmd, def->tpms[i]) < 0)
                return -1;
        } else if (qemuBuildTPMCommandLine(cmd, def,
                                           def->tpms[i], qemuCaps) < 0) {
            return -1;
        }
    }

    return 0;
}


static int
qemuBuildSEVCommandLine(virDomainObj *vm, virCommand *cmd,
                        virDomainSEVDef *sev)
{
    g_autoptr(virJSONValue) props = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    qemuDomainObjPrivate *priv = vm->privateData;
    g_autofree char *dhpath = NULL;
    g_autofree char *sessionpath = NULL;

    VIR_DEBUG("policy=0x%x cbitpos=%d reduced_phys_bits=%d",
              sev->policy, sev->cbitpos, sev->reduced_phys_bits);

    if (sev->dh_cert)
        dhpath = g_strdup_printf("%s/dh_cert.base64", priv->libDir);

    if (sev->session)
        sessionpath = g_strdup_printf("%s/session.base64", priv->libDir);

    if (qemuMonitorCreateObjectProps(&props, "sev-guest", "lsec0",
                                     "u:cbitpos", sev->cbitpos,
                                     "u:reduced-phys-bits", sev->reduced_phys_bits,
                                     "u:policy", sev->policy,
                                     "S:dh-cert-file", dhpath,
                                     "S:session-file", sessionpath,
                                     NULL) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, priv->qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);
    return 0;
}


static int
qemuBuildPVCommandLine(virDomainObj *vm, virCommand *cmd)
{
    g_autoptr(virJSONValue) props = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    qemuDomainObjPrivate *priv = vm->privateData;

    if (qemuMonitorCreateObjectProps(&props, "s390-pv-guest", "lsec0",
                                     NULL) < 0)
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, priv->qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);
    return 0;
}


static int
qemuBuildSecCommandLine(virDomainObj *vm, virCommand *cmd,
                        virDomainSecDef *sec)
{
    if (!sec)
        return 0;

    switch ((virDomainLaunchSecurity) sec->sectype) {
    case VIR_DOMAIN_LAUNCH_SECURITY_SEV:
        return qemuBuildSEVCommandLine(vm, cmd, &sec->data.sev);
        break;
    case VIR_DOMAIN_LAUNCH_SECURITY_PV:
        return qemuBuildPVCommandLine(vm, cmd);
        break;
    case VIR_DOMAIN_LAUNCH_SECURITY_NONE:
    case VIR_DOMAIN_LAUNCH_SECURITY_LAST:
        virReportEnumRangeError(virDomainLaunchSecurity, sec->sectype);
        return -1;
    }

    return 0;
}


static int
qemuBuildVMCoreInfoCommandLine(virCommand *cmd,
                               const virDomainDef *def)
{
    virTristateSwitch vmci = def->features[VIR_DOMAIN_FEATURE_VMCOREINFO];

    if (vmci != VIR_TRISTATE_SWITCH_ON)
        return 0;

    virCommandAddArgList(cmd, "-device", "vmcoreinfo", NULL);
    return 0;
}


static int
qemuBuildPanicCommandLine(virCommand *cmd,
                          const virDomainDef *def)
{
    size_t i;

    for (i = 0; i < def->npanics; i++) {
        switch ((virDomainPanicModel) def->panics[i]->model) {
        case VIR_DOMAIN_PANIC_MODEL_ISA:
            switch (def->panics[i]->info.type) {
            case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA:
                virCommandAddArg(cmd, "-device");
                virCommandAddArgFormat(cmd, "pvpanic,ioport=%d",
                                       def->panics[i]->info.addr.isa.iobase);
                break;

            case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE:
                virCommandAddArgList(cmd, "-device", "pvpanic", NULL);
                break;
            }

        case VIR_DOMAIN_PANIC_MODEL_S390:
        case VIR_DOMAIN_PANIC_MODEL_HYPERV:
        case VIR_DOMAIN_PANIC_MODEL_PSERIES:
        /* default model value was changed before in post parse */
        case VIR_DOMAIN_PANIC_MODEL_DEFAULT:
        case VIR_DOMAIN_PANIC_MODEL_LAST:
            break;
        }
    }

    return 0;
}


static virJSONValue *
qemuBuildPRManagerInfoPropsInternal(const char *alias,
                                    const char *path)
{
    virJSONValue *ret = NULL;

    if (qemuMonitorCreateObjectProps(&ret,
                                     "pr-manager-helper", alias,
                                     "s:path", path, NULL) < 0)
        return NULL;

    return ret;
}


/**
 * qemuBuildPRManagedManagerInfoProps:
 *
 * Build the JSON properties for the pr-manager object corresponding to the PR
 * daemon managed by libvirt.
 */
virJSONValue *
qemuBuildPRManagedManagerInfoProps(qemuDomainObjPrivate *priv)
{
    g_autofree char *path = NULL;

    if (!(path = qemuDomainGetManagedPRSocketPath(priv)))
        return NULL;

    return qemuBuildPRManagerInfoPropsInternal(qemuDomainGetManagedPRAlias(),
                                               path);
}


/**
 * qemuBuildPRManagerInfoProps:
 * @src: storage source
 *
 * Build the JSON properties for the pr-manager object.
 */
virJSONValue *
qemuBuildPRManagerInfoProps(virStorageSource *src)
{
    return qemuBuildPRManagerInfoPropsInternal(src->pr->mgralias, src->pr->path);
}


static int
qemuBuildManagedPRCommandLine(virCommand *cmd,
                              const virDomainDef *def,
                              qemuDomainObjPrivate *priv)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) props = NULL;

    if (!virDomainDefHasManagedPR(def))
        return 0;

    if (!(props = qemuBuildPRManagedManagerInfoProps(priv)))
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, priv->qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    return 0;
}


static int
qemuBuildPflashBlockdevOne(virCommand *cmd,
                           virStorageSource *src,
                           virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;
    size_t i;

    if (!(data = qemuBuildStorageSourceChainAttachPrepareBlockdev(src,
                                                                  qemuCaps)))
        return -1;

    for (i = data->nsrcdata; i > 0; i--) {
        if (qemuBuildBlockStorageSourceAttachDataCommandline(cmd,
                                                             data->srcdata[i - 1],
                                                             qemuCaps) < 0)
            return -1;
    }

    return 0;
}


static int
qemuBuildPflashBlockdevCommandLine(virCommand *cmd,
                                   qemuDomainObjPrivate *priv)
{
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV))
        return 0;

    if (priv->pflash0 &&
        qemuBuildPflashBlockdevOne(cmd, priv->pflash0, priv->qemuCaps) < 0)
        return -1;

    if (priv->pflash1 &&
        qemuBuildPflashBlockdevOne(cmd, priv->pflash1, priv->qemuCaps) < 0)
        return -1;

    return 0;
}


virJSONValue *
qemuBuildDBusVMStateInfoProps(virQEMUDriver *driver,
                              virDomainObj *vm)
{
    virJSONValue *ret = NULL;
    const char *alias = qemuDomainGetDBusVMStateAlias();
    g_autofree char *addr = qemuDBusGetAddress(driver, vm);

    if (!addr)
        return NULL;

    qemuMonitorCreateObjectProps(&ret,
                                 "dbus-vmstate", alias,
                                 "s:addr", addr, NULL);
    return ret;
}


static int
qemuBuildDBusVMStateCommandLine(virCommand *cmd,
                                virQEMUDriver *driver,
                                virDomainObj *vm)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) props = NULL;
    qemuDomainObjPrivate *priv = QEMU_DOMAIN_PRIVATE(vm);

    if (!priv->dbusVMStateIds)
        return 0;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DBUS_VMSTATE)) {
        VIR_INFO("dbus-vmstate object is not supported by this QEMU binary");
        return 0;
    }

    if (!(props = qemuBuildDBusVMStateInfoProps(driver, vm)))
        return -1;

    if (qemuBuildObjectCommandlineFromJSON(&buf, props, priv->qemuCaps) < 0)
        return -1;

    virCommandAddArg(cmd, "-object");
    virCommandAddArgBuffer(cmd, &buf);

    priv->dbusVMState = true;

    return 0;
}


/**
 * qemuBuildCommandLineValidate:
 *
 * Prior to taking the plunge and building a long command line only
 * to find some configuration option isn't valid, let's do a couple
 * of checks and fail early.
 *
 * Returns 0 on success, returns -1 and messages what the issue is.
 */
static int
qemuBuildCommandLineValidate(virQEMUDriver *driver,
                             const virDomainDef *def)
{
    size_t i;
    int sdl = 0;
    int vnc = 0;
    int spice = 0;
    int egl_headless = 0;

    if (!driver->privileged) {
        /* If we have no cgroups then we can have no tunings that
         * require them */

        if (virMemoryLimitIsSet(def->mem.hard_limit) ||
            virMemoryLimitIsSet(def->mem.soft_limit) ||
            virMemoryLimitIsSet(def->mem.swap_hard_limit)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Memory tuning is not available in session mode"));
            return -1;
        }

        if (def->blkio.weight) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Block I/O tuning is not available in session mode"));
            return -1;
        }

        if (def->cputune.sharesSpecified || def->cputune.period ||
            def->cputune.quota || def->cputune.global_period ||
            def->cputune.global_quota || def->cputune.emulator_period ||
            def->cputune.emulator_quota || def->cputune.iothread_period ||
            def->cputune.iothread_quota) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("CPU tuning is not available in session mode"));
            return -1;
        }
    }

    for (i = 0; i < def->ngraphics; ++i) {
        switch (def->graphics[i]->type) {
        case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
            ++sdl;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
            ++vnc;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
            ++spice;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
            ++egl_headless;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
        case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
            break;
        }
    }

    if (sdl > 1 || vnc > 1 || spice > 1 || egl_headless > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only 1 graphics device of each type "
                         "(sdl, vnc, spice, headless) is supported"));
        return -1;
    }

    if (def->virtType == VIR_DOMAIN_VIRT_XEN ||
        def->os.type == VIR_DOMAIN_OSTYPE_XEN ||
        def->os.type == VIR_DOMAIN_OSTYPE_LINUX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("qemu emulator '%s' does not support xen"),
                       def->emulator);
        return -1;
    }

    return 0;
}


static int
qemuBuildSeccompSandboxCommandLine(virCommand *cmd,
                                   virQEMUDriverConfig *cfg,
                                   virQEMUCaps *qemuCaps G_GNUC_UNUSED)
{
    if (cfg->seccompSandbox == 0) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX))
            virCommandAddArgList(cmd, "-sandbox", "off", NULL);
        return 0;
    }

    /* Use blacklist by default if supported */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SECCOMP_BLACKLIST)) {
        virCommandAddArgList(cmd, "-sandbox",
                             "on,obsolete=deny,elevateprivileges=deny,"
                             "spawn=deny,resourcecontrol=deny",
                             NULL);
        return 0;
    }

    /* Seccomp whitelist is opt-in */
    if (cfg->seccompSandbox > 0)
        virCommandAddArgList(cmd, "-sandbox", "on", NULL);

    return 0;

}


char *
qemuBuildVsockDevStr(virDomainDef *def,
                     virDomainVsockDef *vsock,
                     virQEMUCaps *qemuCaps,
                     const char *fdprefix)
{
    qemuDomainVsockPrivate *priv = (qemuDomainVsockPrivate *)vsock->privateData;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (qemuBuildVirtioDevStr(&buf, "vhost-vsock", qemuCaps,
                              VIR_DOMAIN_DEVICE_VSOCK, vsock) < 0) {
        return NULL;
    }

    virBufferAsprintf(&buf, ",id=%s", vsock->info.alias);
    virBufferAsprintf(&buf, ",guest-cid=%u", vsock->guest_cid);
    virBufferAsprintf(&buf, ",vhostfd=%s%u", fdprefix, priv->vhostfd);

    qemuBuildVirtioOptionsStr(&buf, vsock->virtio);

    if (qemuBuildDeviceAddressStr(&buf, def, &vsock->info) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static int
qemuBuildVsockCommandLine(virCommand *cmd,
                          virDomainDef *def,
                          virDomainVsockDef *vsock,
                          virQEMUCaps *qemuCaps)
{
    qemuDomainVsockPrivate *priv = (qemuDomainVsockPrivate *)vsock->privateData;
    g_autofree char *devstr = NULL;

    if (!(devstr = qemuBuildVsockDevStr(def, vsock, qemuCaps, "")))
        return -1;

    virCommandPassFD(cmd, priv->vhostfd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
    priv->vhostfd = -1;

    if (qemuCommandAddExtDevice(cmd, &vsock->info) < 0)
        return -1;

    virCommandAddArgList(cmd, "-device", devstr, NULL);

    return 0;
}


typedef enum {
    QEMU_COMMAND_DEPRECATION_BEHAVIOR_NONE = 0,
    QEMU_COMMAND_DEPRECATION_BEHAVIOR_OMIT,
    QEMU_COMMAND_DEPRECATION_BEHAVIOR_REJECT,
    QEMU_COMMAND_DEPRECATION_BEHAVIOR_CRASH,

    QEMU_COMMAND_DEPRECATION_BEHAVIOR_LAST
} qemuCommnadDeprecationBehavior;


VIR_ENUM_DECL(qemuCommnadDeprecationBehavior);
VIR_ENUM_IMPL(qemuCommnadDeprecationBehavior,
              QEMU_COMMAND_DEPRECATION_BEHAVIOR_LAST,
              "none",
              "omit",
              "reject",
              "crash");

static void
qemuBuildCompatDeprecatedCommandLine(virCommand *cmd,
                                     virQEMUDriverConfig *cfg,
                                     virDomainDef *def,
                                     virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    qemuDomainXmlNsDef *nsdata = def->namespaceData;
    qemuCommnadDeprecationBehavior behavior = QEMU_COMMAND_DEPRECATION_BEHAVIOR_NONE;
    const char *behaviorStr = cfg->deprecationBehavior;
    int tmp;

    if (nsdata && nsdata->deprecationBehavior)
        behaviorStr = nsdata->deprecationBehavior;

    if ((tmp = qemuCommnadDeprecationBehaviorTypeFromString(behaviorStr)) < 0) {
        VIR_WARN("Unsupported deprecation behavior '%s' for VM '%s'",
                 behaviorStr, def->name);
        return;
    }

    behavior = tmp;

    if (behavior == QEMU_COMMAND_DEPRECATION_BEHAVIOR_NONE)
        return;

    /* we don't try to enable this feature at all if qemu doesn't support it,
     * so that a downgrade of qemu version doesn't impact startup of the VM */
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_COMPAT_DEPRECATED)) {
        VIR_DEBUG("-compat not supported for VM '%s'", def->name);
        return;
    }

    /* all active options hide output fields from qemu */
    virBufferAddLit(&buf, "deprecated-output=hide,");

    switch (behavior) {
    case QEMU_COMMAND_DEPRECATION_BEHAVIOR_OMIT:
    case QEMU_COMMAND_DEPRECATION_BEHAVIOR_NONE:
    case QEMU_COMMAND_DEPRECATION_BEHAVIOR_LAST:
    default:
        /* output field hiding is default for all cases */
        break;

    case QEMU_COMMAND_DEPRECATION_BEHAVIOR_REJECT:
        virBufferAddLit(&buf, "deprecated-input=reject,");
        break;

    case QEMU_COMMAND_DEPRECATION_BEHAVIOR_CRASH:
        virBufferAddLit(&buf, "deprecated-input=crash,");
        break;
    }

    virBufferTrim(&buf, ",");

    virCommandAddArg(cmd, "-compat");
    virCommandAddArgBuffer(cmd, &buf);
}


/*
 * Constructs a argv suitable for launching qemu with config defined
 * for a given virtual machine.
 */
virCommand *
qemuBuildCommandLine(virQEMUDriver *driver,
                     virLogManager *logManager,
                     virSecurityManager *secManager,
                     virDomainObj *vm,
                     const char *migrateURI,
                     virDomainMomentObj *snapshot,
                     virNetDevVPortProfileOp vmop,
                     bool standalone,
                     bool enableFips,
                     size_t *nnicindexes,
                     int **nicindexes,
                     unsigned int flags)
{
    size_t i;
    char uuid[VIR_UUID_STRING_BUFLEN];
    g_autoptr(virCommand) cmd = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    unsigned int bootHostdevNet = 0;
    qemuDomainObjPrivate *priv = vm->privateData;
    virDomainDef *def = vm->def;
    virQEMUCaps *qemuCaps = priv->qemuCaps;
    bool chardevStdioLogd = priv->chardevStdioLogd;

    VIR_DEBUG("driver=%p def=%p mon=%p "
              "qemuCaps=%p migrateURI=%s snapshot=%p vmop=%d flags=0x%x",
              driver, def, priv->monConfig,
              qemuCaps, migrateURI, snapshot, vmop, flags);

    if (qemuBuildCommandLineValidate(driver, def) < 0)
        return NULL;

    cmd = virCommandNew(def->emulator);

    virCommandAddEnvPassCommon(cmd);

    /* For system QEMU we want to set both HOME and all the XDG variables to
     * libDir/qemu otherwise apps QEMU links to might try to access the default
     * home dir '/' which would always result in a permission issue.
     *
     * For session QEMU, we only want to set XDG_CACHE_HOME as cache data
     * may be purged at any time and that should not affect any app. We
     * do want VMs to integrate with services in user's session so we're
     * not re-setting any other env variables
     */
    if (!driver->privileged) {
        virCommandAddEnvFormat(cmd, "XDG_CACHE_HOME=%s/%s",
                               priv->libDir, ".cache");
    } else {
        virCommandAddEnvPair(cmd, "HOME", priv->libDir);
        virCommandAddEnvXDG(cmd, priv->libDir);
    }

    if (qemuBuildNameCommandLine(cmd, cfg, def) < 0)
        return NULL;

    qemuBuildCompatDeprecatedCommandLine(cmd, cfg, def, qemuCaps);

    if (!standalone)
        virCommandAddArg(cmd, "-S"); /* freeze CPU */

    if (qemuBuildMasterKeyCommandLine(cmd, priv) < 0)
        return NULL;

    if (qemuBuildDBusVMStateCommandLine(cmd, driver, vm) < 0)
        return NULL;

    if (qemuBuildManagedPRCommandLine(cmd, def, priv) < 0)
        return NULL;

    if (qemuBuildPflashBlockdevCommandLine(cmd, priv) < 0)
        return NULL;

    if (enableFips)
        virCommandAddArg(cmd, "-enable-fips");

    if (qemuBuildMachineCommandLine(cmd, cfg, def, qemuCaps, priv) < 0)
        return NULL;

    qemuBuildTSEGCommandLine(cmd, def);

    if (qemuBuildCpuCommandLine(cmd, driver, def, qemuCaps) < 0)
        return NULL;

    qemuBuildDomainLoaderCommandLine(cmd, def, qemuCaps);

    if (qemuBuildMemCommandLine(cmd, def, qemuCaps, priv) < 0)
        return NULL;

    if (qemuBuildSmpCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildIOThreadCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (virDomainNumaGetNodeCount(def->numa) &&
        qemuBuildNumaCommandLine(cfg, def, cmd, priv) < 0)
        return NULL;

    if (qemuBuildMemoryDeviceCommandLine(cmd, cfg, def, priv) < 0)
        return NULL;

    virUUIDFormat(def->uuid, uuid);
    virCommandAddArgList(cmd, "-uuid", uuid, NULL);

    if (qemuBuildSmbiosCommandLine(cmd, driver, def) < 0)
        return NULL;

    if (qemuBuildSysinfoCommandLine(cmd, def) < 0)
        return NULL;

    if (qemuBuildVMGenIDCommandLine(cmd, def) < 0)
        return NULL;

    /*
     * NB, -nographic *MUST* come before any serial, or monitor
     * or parallel port flags due to QEMU craziness, where it
     * decides to change the serial port & monitor to be on stdout
     * if you ask for nographic. So we have to make sure we override
     * these defaults ourselves...
     */
    if (!def->ngraphics) {
        virCommandAddArg(cmd, "-display");
        virCommandAddArg(cmd, "none");
    }

    /* Disable global config files and default devices */
    virCommandAddArg(cmd, "-no-user-config");
    virCommandAddArg(cmd, "-nodefaults");

    if (qemuBuildSgaCommandLine(cmd, def) < 0)
        return NULL;

    if (qemuBuildMonitorCommandLine(logManager, secManager, cmd, cfg, def, priv) < 0)
        return NULL;

    if (qemuBuildClockCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildPMCommandLine(cmd, def, priv) < 0)
        return NULL;

    if (qemuBuildBootCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildIOMMUCommandLine(cmd, def) < 0)
        return NULL;

    if (qemuBuildGlobalControllerCommandLine(cmd, def) < 0)
        return NULL;

    if (qemuBuildControllersCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildHubCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildControllersByTypeCommandLine(cmd, def, qemuCaps,
                                              VIR_DOMAIN_CONTROLLER_TYPE_CCID) < 0)
        return NULL;

    if (qemuBuildDisksCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildFilesystemCommandLine(cmd, def, qemuCaps, priv) < 0)
        return NULL;

    if (qemuBuildNetCommandLine(driver, vm, logManager, secManager, cmd,
                                qemuCaps, vmop, standalone,
                                nnicindexes, nicindexes, &bootHostdevNet, flags) < 0)
        return NULL;

    if (qemuBuildSmartcardCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                      chardevStdioLogd) < 0)
        return NULL;

    if (qemuBuildSerialCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                   chardevStdioLogd) < 0)
        return NULL;

    if (qemuBuildParallelsCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                      chardevStdioLogd) < 0)
        return NULL;

    if (qemuBuildChannelsCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                     chardevStdioLogd, flags) < 0)
        return NULL;

    if (qemuBuildConsoleCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                    chardevStdioLogd) < 0)
        return NULL;

    if (qemuBuildTPMsCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildInputCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildAudioCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildGraphicsCommandLine(cfg, cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildVideoCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildSoundCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildWatchdogCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildRedirdevCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                     chardevStdioLogd) < 0)
        return NULL;

    if (qemuBuildHostdevCommandLine(cmd, def, qemuCaps, &bootHostdevNet) < 0)
        return NULL;

    if (migrateURI)
        virCommandAddArgList(cmd, "-incoming", migrateURI, NULL);

    if (qemuBuildMemballoonCommandLine(cmd, def, qemuCaps) < 0)
        return NULL;

    if (qemuBuildRNGCommandLine(logManager, secManager, cmd, cfg, def, qemuCaps,
                                chardevStdioLogd) < 0)
        return NULL;

    if (qemuBuildNVRAMCommandLine(cmd, def) < 0)
        return NULL;

    if (qemuBuildVMCoreInfoCommandLine(cmd, def) < 0)
        return NULL;

    if (qemuBuildSecCommandLine(vm, cmd, def->sec) < 0)
        return NULL;

    if (snapshot)
        virCommandAddArgList(cmd, "-loadvm", snapshot->def->name, NULL);

    if (def->namespaceData) {
        qemuDomainXmlNsDef *qemuxmlns;
        GStrv n;

        qemuxmlns = def->namespaceData;
        for (n = qemuxmlns->args; n && *n; n++)
            virCommandAddArg(cmd, *n);
        for (i = 0; i < qemuxmlns->num_env; i++)
            virCommandAddEnvPair(cmd, qemuxmlns->env[i].name,
                                 NULLSTR_EMPTY(qemuxmlns->env[i].value));
    }

    if (qemuBuildSeccompSandboxCommandLine(cmd, cfg, qemuCaps) < 0)
        return NULL;

    if (qemuBuildPanicCommandLine(cmd, def) < 0)
        return NULL;

    for (i = 0; i < def->nshmems; i++) {
        if (qemuBuildShmemCommandLine(logManager, secManager, cmd, cfg,
                                      def, def->shmems[i], qemuCaps,
                                      chardevStdioLogd))
            return NULL;
    }

    if (def->vsock &&
        qemuBuildVsockCommandLine(cmd, def, def->vsock, qemuCaps) < 0)
        return NULL;

    if (cfg->logTimestamp)
        virCommandAddArgList(cmd, "-msg", "timestamp=on", NULL);

    return g_steal_pointer(&cmd);
}


/* This function generates the correct '-device' string for character
 * devices of each architecture.
 */
static int
qemuBuildSerialChrDeviceStr(char **deviceStr,
                            const virDomainDef *def,
                            virDomainChrDef *serial,
                            virQEMUCaps *qemuCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    virQEMUCapsFlags caps;

    switch ((virDomainChrSerialTargetModel) serial->targetModel) {
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE:

        caps = qemuChrSerialTargetModelToCaps(serial->targetModel);

        if (caps && !virQEMUCapsGet(qemuCaps, caps)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("'%s' is not supported in this QEMU binary"),
                           virDomainChrSerialTargetModelTypeToString(serial->targetModel));
            return -1;
        }
        break;

    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_16550A:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_LAST:
        /* Except from _LAST, which is just a guard value and will never
         * be used, all of the above are platform devices, which means
         * qemuBuildSerialCommandLine() will have taken the appropriate
         * branch and we will not have ended up here. */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Invalid target model for serial device"));
        return -1;
    }

    virBufferAsprintf(&buf, "%s,chardev=char%s,id=%s",
                      virDomainChrSerialTargetModelTypeToString(serial->targetModel),
                      serial->info.alias, serial->info.alias);

    if (qemuBuildDeviceAddressStr(&buf, def, &serial->info) < 0)
        return -1;

    *deviceStr = virBufferContentAndReset(&buf);
    return 0;
}

static int
qemuBuildParallelChrDeviceStr(char **deviceStr,
                              virDomainChrDef *chr)
{
    *deviceStr = g_strdup_printf("isa-parallel,chardev=char%s,id=%s",
                                 chr->info.alias, chr->info.alias);
    return 0;
}


virJSONValue *
qemuBuildChannelGuestfwdNetdevProps(virDomainChrDef *chr)
{
    g_autoptr(virJSONValue) guestfwdarr = virJSONValueNewArray();
    g_autoptr(virJSONValue) guestfwdstrobj = virJSONValueNewObject();
    g_autofree char *addr = NULL;
    virJSONValue *ret = NULL;

    if (!(addr = virSocketAddrFormat(chr->target.addr)))
        return NULL;

    /* this may seem weird, but qemu indeed decided that 'guestfwd' parameter
     * is an array of objects which have just one member named 'str' which
     * contains the description */
    if (virJSONValueObjectAppendStringPrintf(guestfwdstrobj, "str",
                                             "tcp:%s:%i-chardev:char%s",
                                             addr,
                                             virSocketAddrGetPort(chr->target.addr),
                                             chr->info.alias) < 0)
        return NULL;

    if (virJSONValueArrayAppend(guestfwdarr, &guestfwdstrobj) < 0)
        return NULL;

    if (virJSONValueObjectCreate(&ret,
                                 "s:type", "user",
                                 "a:guestfwd", &guestfwdarr,
                                 "s:id", chr->info.alias,
                                 NULL) < 0)
        return NULL;

    return ret;
}


static int
qemuBuildChannelChrDeviceStr(char **deviceStr,
                             const virDomainDef *def,
                             virDomainChrDef *chr)
{
    switch ((virDomainChrChannelTargetType)chr->targetType) {
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
        if (!(*deviceStr = qemuBuildVirtioSerialPortDevStr(def, chr)))
            return -1;
        break;

    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
        /* guestfwd is as a netdev handled separately */
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_XEN:
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_NONE:
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_LAST:
        return -1;
    }

    return 0;
}

static int
qemuBuildConsoleChrDeviceStr(char **deviceStr,
                             const virDomainDef *def,
                             virDomainChrDef *chr)
{
    switch ((virDomainChrConsoleTargetType)chr->targetType) {
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
        if (!(*deviceStr = qemuBuildSclpDevStr(chr)))
            return -1;
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO:
        if (!(*deviceStr = qemuBuildVirtioSerialPortDevStr(def, chr)))
            return -1;
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL:
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_UML:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LXC:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_OPENVZ:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported console target type %s"),
                       NULLSTR(virDomainChrConsoleTargetTypeToString(chr->targetType)));
        return -1;
    }

    return 0;
}

int
qemuBuildChrDeviceStr(char **deviceStr,
                      const virDomainDef *vmdef,
                      virDomainChrDef *chr,
                      virQEMUCaps *qemuCaps)
{
    int ret = -1;

    switch ((virDomainChrDeviceType)chr->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        ret = qemuBuildSerialChrDeviceStr(deviceStr, vmdef, chr, qemuCaps);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
        ret = qemuBuildParallelChrDeviceStr(deviceStr, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        ret = qemuBuildChannelChrDeviceStr(deviceStr, vmdef, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        ret = qemuBuildConsoleChrDeviceStr(deviceStr, vmdef, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        return ret;
    }

    return ret;
}


virJSONValue *
qemuBuildHotpluggableCPUProps(const virDomainVcpuDef *vcpu)
{
    qemuDomainVcpuPrivate *vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);
    g_autoptr(virJSONValue) ret = NULL;

    if (!(ret = virJSONValueCopy(vcpupriv->props)))
        return NULL;

    if (virJSONValueObjectPrependString(ret, "id", vcpupriv->alias) < 0 ||
        virJSONValueObjectPrependString(ret, "driver", vcpupriv->type) < 0)
        return NULL;

    return g_steal_pointer(&ret);
}


/**
 * qemuBuildStorageSourceAttachPrepareDrive:
 * @disk: disk object to prepare
 * @qemuCaps: qemu capabilities object
 * @driveBoot: bootable flag for disks which don't have -device part
 *
 * Prepare qemuBlockStorageSourceAttachData *for use with the old approach
 * using -drive/drive_add. See qemuBlockStorageSourceAttachPrepareBlockdev.
 */
qemuBlockStorageSourceAttachData *
qemuBuildStorageSourceAttachPrepareDrive(virDomainDiskDef *disk,
                                         virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceAttachData) data = NULL;

    data = g_new0(qemuBlockStorageSourceAttachData, 1);

    if (!(data->driveCmd = qemuBuildDriveStr(disk, qemuCaps)) ||
        !(data->driveAlias = qemuAliasDiskDriveFromDisk(disk)))
        return NULL;

    return g_steal_pointer(&data);
}


/**
 * qemuBuildStorageSourceAttachPrepareChardev:
 * @src: disk source to prepare
 *
 * Prepare qemuBlockStorageSourceAttachData *for vhost-user disk
 * to be used with -chardev.
 */
qemuBlockStorageSourceAttachData *
qemuBuildStorageSourceAttachPrepareChardev(virDomainDiskDef *disk)
{
    g_autoptr(qemuBlockStorageSourceAttachData) data = NULL;
    g_auto(virBuffer) chardev = VIR_BUFFER_INITIALIZER;

    data = g_new0(qemuBlockStorageSourceAttachData, 1);

    data->chardevDef = disk->src->vhostuser;
    data->chardevAlias = qemuDomainGetVhostUserChrAlias(disk->info.alias);

    virBufferAddLit(&chardev, "socket");
    virBufferAsprintf(&chardev, ",id=%s", data->chardevAlias);
    virBufferAddLit(&chardev, ",path=");
    virQEMUBuildBufferEscapeComma(&chardev, disk->src->vhostuser->data.nix.path);

    qemuBuildChrChardevReconnectStr(&chardev,
                                    &disk->src->vhostuser->data.nix.reconnect);

    if (!(data->chardevCmd = virBufferContentAndReset(&chardev)))
        return NULL;

    return g_steal_pointer(&data);
}


/**
 * qemuBuildStorageSourceAttachPrepareCommon:
 * @src: storage source
 * @data: already initialized data for disk source addition
 * @qemuCaps: qemu capabilities object
 *
 * Prepare data for configuration associated with the disk source such as
 * secrets/TLS/pr objects etc ...
 */
int
qemuBuildStorageSourceAttachPrepareCommon(virStorageSource *src,
                                          qemuBlockStorageSourceAttachData *data,
                                          virQEMUCaps *qemuCaps)
{
    qemuDomainStorageSourcePrivate *srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);
    const char *tlsKeySecretAlias = NULL;

    if (src->pr &&
        !virStoragePRDefIsManaged(src->pr) &&
        !(data->prmgrProps = qemuBuildPRManagerInfoProps(src)))
        return -1;

    if (srcpriv) {
        if (srcpriv->secinfo &&
            srcpriv->secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES &&
            qemuBuildSecretInfoProps(srcpriv->secinfo, &data->authsecretProps) < 0)
            return -1;

        if (srcpriv->encinfo &&
            qemuBuildSecretInfoProps(srcpriv->encinfo, &data->encryptsecretProps) < 0)
            return -1;

        if (srcpriv->httpcookie &&
            qemuBuildSecretInfoProps(srcpriv->httpcookie, &data->httpcookiesecretProps) < 0)
            return -1;

        if (srcpriv->tlsKeySecret) {
            if (qemuBuildSecretInfoProps(srcpriv->tlsKeySecret, &data->tlsKeySecretProps) < 0)
                return -1;

            tlsKeySecretAlias = srcpriv->tlsKeySecret->s.aes.alias;
        }
    }

    if (src->haveTLS == VIR_TRISTATE_BOOL_YES &&
        qemuBuildTLSx509BackendProps(src->tlsCertdir, false, true, src->tlsAlias,
                                     tlsKeySecretAlias, qemuCaps, &data->tlsProps) < 0)
        return -1;

    return 0;
}


/**
 * qemuBuildStorageSourceChainAttachPrepareDrive:
 * @disk: disk definition
 * @qemuCaps: qemu capabilities object
 *
 * Prepares qemuBlockStorageSourceChainData *for attaching @disk via -drive.
 */
qemuBlockStorageSourceChainData *
qemuBuildStorageSourceChainAttachPrepareDrive(virDomainDiskDef *disk,
                                              virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceAttachData) elem = NULL;
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;

    data = g_new0(qemuBlockStorageSourceChainData, 1);

    if (!(elem = qemuBuildStorageSourceAttachPrepareDrive(disk, qemuCaps)))
        return NULL;

    if (qemuBuildStorageSourceAttachPrepareCommon(disk->src, elem, qemuCaps) < 0)
        return NULL;

    VIR_APPEND_ELEMENT(data->srcdata, data->nsrcdata, elem);

    return g_steal_pointer(&data);
}


/**
 * qemuBuildStorageSourceChainAttachPrepareChardev:
 * @src: disk definition
 *
 * Prepares qemuBlockStorageSourceChainData *for attaching a vhost-user
 * disk's backend via -chardev.
 */
qemuBlockStorageSourceChainData *
qemuBuildStorageSourceChainAttachPrepareChardev(virDomainDiskDef *disk)
{
    g_autoptr(qemuBlockStorageSourceAttachData) elem = NULL;
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;

    data = g_new0(qemuBlockStorageSourceChainData, 1);

    if (!(elem = qemuBuildStorageSourceAttachPrepareChardev(disk)))
        return NULL;

    VIR_APPEND_ELEMENT(data->srcdata, data->nsrcdata, elem);

    return g_steal_pointer(&data);
}


static int
qemuBuildStorageSourceChainAttachPrepareBlockdevOne(qemuBlockStorageSourceChainData *data,
                                                    virStorageSource *src,
                                                    virStorageSource *backingStore,
                                                    virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceAttachData) elem = NULL;

    if (!(elem = qemuBlockStorageSourceAttachPrepareBlockdev(src, backingStore, true)))
        return -1;

    if (qemuBuildStorageSourceAttachPrepareCommon(src, elem, qemuCaps) < 0)
        return -1;

    VIR_APPEND_ELEMENT(data->srcdata, data->nsrcdata, elem);

    return 0;
}


/**
 * qemuBuildStorageSourceChainAttachPrepareBlockdev:
 * @top: storage source chain
 * @qemuCaps: qemu capabilities object
 *
 * Prepares qemuBlockStorageSourceChainData *for attaching the chain of images
 * starting at @top via -blockdev.
 */
qemuBlockStorageSourceChainData *
qemuBuildStorageSourceChainAttachPrepareBlockdev(virStorageSource *top,
                                                 virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;
    virStorageSource *n;

    data = g_new0(qemuBlockStorageSourceChainData, 1);

    for (n = top; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (qemuBuildStorageSourceChainAttachPrepareBlockdevOne(data, n,
                                                                n->backingStore,
                                                                qemuCaps) < 0)
            return NULL;
    }

    return g_steal_pointer(&data);
}


/**
 * qemuBuildStorageSourceChainAttachPrepareBlockdevTop:
 * @top: storage source chain
 * @backingStore: a storage source to use as backing of @top
 * @qemuCaps: qemu capabilities object
 *
 * Prepares qemuBlockStorageSourceChainData *for attaching of @top image only
 * via -blockdev.
 */
qemuBlockStorageSourceChainData *
qemuBuildStorageSourceChainAttachPrepareBlockdevTop(virStorageSource *top,
                                                    virStorageSource *backingStore,
                                                    virQEMUCaps *qemuCaps)
{
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;

    data = g_new0(qemuBlockStorageSourceChainData, 1);

    if (qemuBuildStorageSourceChainAttachPrepareBlockdevOne(data, top, backingStore,
                                                            qemuCaps) < 0)
        return NULL;

    return g_steal_pointer(&data);
}
