/*
 * virsh-nodedev.c: Commands in node device group
 *
 * Copyright (C) 2005, 2007-2016 Red Hat, Inc.
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
#include "virsh-nodedev.h"

#include "internal.h"
#include "viralloc.h"
#include "virfile.h"
#include "virstring.h"
#include "virtime.h"
#include "conf/node_device_conf.h"
#include "virenum.h"
#include "virutil.h"

/*
 * "nodedev-create" command
 */
static const vshCmdInfo info_node_device_create[] = {
    {.name = "help",
     .data = N_("create a device defined "
                "by an XML file on the node")
    },
    {.name = "desc",
     .data = N_("Create a device on the node.  Note that this "
                "command creates devices on the physical host "
                "that can then be assigned to a virtual machine.")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_device_create[] = {
    VIRSH_COMMON_OPT_FILE(N_("file containing an XML description "
                             "of the device")),
    {.name = NULL}
};

static bool
cmdNodeDeviceCreate(vshControl *ctl, const vshCmd *cmd)
{
    virNodeDevicePtr dev = NULL;
    const char *from = NULL;
    bool ret = true;
    g_autofree char *buffer = NULL;
    virshControl *priv = ctl->privData;

    if (vshCommandOptStringReq(ctl, cmd, "file", &from) < 0)
        return false;

    if (virFileReadAll(from, VSH_MAX_XML_FILE, &buffer) < 0)
        return false;

    dev = virNodeDeviceCreateXML(priv->conn, buffer, 0);

    if (dev != NULL) {
        vshPrintExtra(ctl, _("Node device %s created from %s\n"),
                      virNodeDeviceGetName(dev), from);
        virNodeDeviceFree(dev);
    } else {
        vshError(ctl, _("Failed to create node device from %s"), from);
        ret = false;
    }

    return ret;
}


/*
 * "nodedev-destroy" command
 */
static const vshCmdInfo info_node_device_destroy[] = {
    {.name = "help",
     .data = N_("destroy (stop) a device on the node")
    },
    {.name = "desc",
     .data = N_("Destroy a device on the node.  Note that this "
                "command destroys devices on the physical host")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_device_destroy[] = {
    {.name = "name",
     .type = VSH_OT_ALIAS,
     .help = "device"
    },
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device name or wwn pair in 'wwnn,wwpn' format"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = NULL}
};

static virNodeDevice*
vshFindNodeDevice(vshControl *ctl, const char *value)
{
    virNodeDevicePtr dev = NULL;
    g_auto(GStrv) arr = NULL;
    int narr;
    virshControl *priv = ctl->privData;

    if (strchr(value, ',')) {
        narr = vshStringToArray(value, &arr);
        if (narr != 2) {
            vshError(ctl, _("Malformed device value '%s'"), value);
            return NULL;
        }

        if (!virValidateWWN(arr[0]) || !virValidateWWN(arr[1]))
            return NULL;

        dev = virNodeDeviceLookupSCSIHostByWWN(priv->conn, arr[0], arr[1], 0);
    } else {
        dev = virNodeDeviceLookupByName(priv->conn, value);
    }

    if (!dev) {
        vshError(ctl, "%s '%s'", _("Could not find matching device"), value);
        return NULL;
    }

    return dev;
}

static bool
cmdNodeDeviceDestroy(vshControl *ctl, const vshCmd *cmd)
{
    virNodeDevice *dev = NULL;
    bool ret = false;
    const char *device_value = NULL;

    if (vshCommandOptStringReq(ctl, cmd, "device", &device_value) < 0)
        return false;

    dev = vshFindNodeDevice(ctl, device_value);
    if (!dev)
        goto cleanup;

    if (virNodeDeviceDestroy(dev) == 0) {
        vshPrintExtra(ctl, _("Destroyed node device '%s'\n"), device_value);
    } else {
        vshError(ctl, _("Failed to destroy node device '%s'"), device_value);
        goto cleanup;
    }

    ret = true;
 cleanup:
    if (dev)
        virNodeDeviceFree(dev);
    return ret;
}

struct virshNodeList {
    char **names;
    char **parents;
};

static const char *
virshNodeListLookup(int devid, bool parent, void *opaque)
{
    struct virshNodeList *arrays = opaque;
    if (parent)
        return arrays->parents[devid];
    return arrays->names[devid];
}

static int
virshNodeDeviceSorter(const void *a, const void *b)
{
    virNodeDevicePtr *na = (virNodeDevicePtr *) a;
    virNodeDevicePtr *nb = (virNodeDevicePtr *) b;

    if (*na && !*nb)
        return -1;

    if (!*na)
        return *nb != NULL;

    return vshStrcasecmp(virNodeDeviceGetName(*na),
                         virNodeDeviceGetName(*nb));
}

struct virshNodeDeviceList {
    virNodeDevicePtr *devices;
    size_t ndevices;
};

static void
virshNodeDeviceListFree(struct virshNodeDeviceList *list)
{
    size_t i;

    if (list && list->devices) {
        for (i = 0; i < list->ndevices; i++) {
            if (list->devices[i])
                virNodeDeviceFree(list->devices[i]);
        }
        g_free(list->devices);
    }
    g_free(list);
}

static struct virshNodeDeviceList *
virshNodeDeviceListCollect(vshControl *ctl,
                         char **capnames,
                         int ncapnames,
                         unsigned int flags)
{
    struct virshNodeDeviceList *list = g_new0(struct virshNodeDeviceList, 1);
    size_t i;
    int ret;
    virNodeDevicePtr device;
    bool success = false;
    size_t deleted = 0;
    int ndevices = 0;
    char **names = NULL;
    virshControl *priv = ctl->privData;

    /* try the list with flags support (0.10.2 and later) */
    if ((ret = virConnectListAllNodeDevices(priv->conn,
                                            &list->devices,
                                            flags)) >= 0) {
        list->ndevices = ret;
        goto finished;
    }

    /* check if the command is actually supported */
    if (last_error && last_error->code == VIR_ERR_NO_SUPPORT)
        goto fallback;

    /* there was an error during the call */
    vshError(ctl, "%s", _("Failed to list node devices"));
    goto cleanup;


 fallback:
    /* fall back to old method (0.10.1 and older) */
    vshResetLibvirtError();

    ndevices = virNodeNumOfDevices(priv->conn, NULL, 0);
    if (ndevices < 0) {
        vshError(ctl, "%s", _("Failed to count node devices"));
        goto cleanup;
    }

    if (ndevices == 0)
        return list;

    names = g_new0(char *, ndevices);

    ndevices = virNodeListDevices(priv->conn, NULL, names, ndevices, 0);
    if (ndevices < 0) {
        vshError(ctl, "%s", _("Failed to list node devices"));
        goto cleanup;
    }

    list->devices = g_new0(virNodeDevicePtr, ndevices);
    list->ndevices = 0;

    /* get the node devices */
    for (i = 0; i < ndevices; i++) {
        if (!(device = virNodeDeviceLookupByName(priv->conn, names[i])))
            continue;
        list->devices[list->ndevices++] = device;
    }

    /* truncate domains that weren't found */
    deleted = ndevices - list->ndevices;

    if (!capnames)
        goto finished;

    /* filter the list if the list was acquired by fallback means */
    for (i = 0; i < list->ndevices; i++) {
        g_autofree char **caps = NULL;
        int ncaps = 0;
        bool match = false;
        size_t j, k;

        device = list->devices[i];

        if ((ncaps = virNodeDeviceNumOfCaps(device)) < 0) {
            vshError(ctl, "%s", _("Failed to get capability numbers "
                                  "of the device"));
            goto cleanup;
        }

        caps = g_new0(char *, ncaps);

        if ((ncaps = virNodeDeviceListCaps(device, caps, ncaps)) < 0) {
            vshError(ctl, "%s", _("Failed to get capability names of the device"));
            goto cleanup;
        }

        /* Check if the device's capability matches with provided
         * capabilities.
         */
        for (j = 0; j < ncaps; j++) {
            for (k = 0; k < ncapnames; k++) {
                if (STREQ(caps[j], capnames[k])) {
                    match = true;
                    break;
                }
            }
        }

        if (!match)
            goto remove_entry;

        /* the device matched all filters, it may stay */
        continue;

 remove_entry:
        /* the device has to be removed as it failed one of the filters */
        virNodeDeviceFree(list->devices[i]);
        list->devices[i] = NULL;
        deleted++;
    }

 finished:
    /* sort the list */
    if (list->devices && list->ndevices)
        qsort(list->devices, list->ndevices,
              sizeof(*list->devices), virshNodeDeviceSorter);

    /* truncate the list if filter simulation deleted entries */
    if (deleted)
        VIR_SHRINK_N(list->devices, list->ndevices, deleted);

    success = true;

 cleanup:
    for (i = 0; ndevices != -1 && i < ndevices; i++)
        VIR_FREE(names[i]);
    VIR_FREE(names);

    if (!success) {
        virshNodeDeviceListFree(list);
        list = NULL;
    }

    return list;
}

/*
 * "nodedev-list" command
 */
static const vshCmdInfo info_node_list_devices[] = {
    {.name = "help",
     .data = N_("enumerate devices on this host")
    },
    {.name = "desc",
     .data = ""
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_list_devices[] = {
    {.name = "tree",
     .type = VSH_OT_BOOL,
     .help = N_("list devices in a tree")
    },
    {.name = "cap",
     .type = VSH_OT_STRING,
     .completer = virshNodeDeviceCapabilityNameCompleter,
     .help = N_("capability names, separated by comma")
    },
    {.name = "inactive",
     .type = VSH_OT_BOOL,
     .help = N_("list inactive devices")
    },
    {.name = "all",
     .type = VSH_OT_BOOL,
     .help = N_("list inactive & active devices")
    },
    {.name = NULL}
};

static bool
cmdNodeListDevices(vshControl *ctl, const vshCmd *cmd G_GNUC_UNUSED)
{
    const char *cap_str = NULL;
    size_t i;
    bool tree = vshCommandOptBool(cmd, "tree");
    bool ret = true;
    unsigned int flags = 0;
    g_auto(GStrv) caps = NULL;
    int ncaps = 0;
    struct virshNodeDeviceList *list = NULL;
    int cap_type = -1;
    bool inactive = vshCommandOptBool(cmd, "inactive");
    bool all = vshCommandOptBool(cmd, "all");

    ignore_value(vshCommandOptStringQuiet(ctl, cmd, "cap", &cap_str));

    if (cap_str) {
        if ((ncaps = vshStringToArray(cap_str, &caps)) < 0)
            return false;
    }

    if (all && inactive) {
        vshError(ctl, "%s", _("Option --all is incompatible with --inactive"));
        return false;
    }

    if (tree && (cap_str || inactive)) {
        vshError(ctl, "%s", _("Option --tree is incompatible with --cap and --inactive"));
        return false;
    }

    for (i = 0; i < ncaps; i++) {
        if ((cap_type = virNodeDevCapTypeFromString(caps[i])) < 0) {
            vshError(ctl, "%s", _("Invalid capability type"));
            ret = false;
            goto cleanup;
        }

        switch ((virNodeDevCapType) cap_type) {
        case VIR_NODE_DEV_CAP_SYSTEM:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_SYSTEM;
            break;
        case VIR_NODE_DEV_CAP_PCI_DEV:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_PCI_DEV;
            break;
        case VIR_NODE_DEV_CAP_USB_DEV:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_USB_DEV;
            break;
        case VIR_NODE_DEV_CAP_USB_INTERFACE:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_USB_INTERFACE;
            break;
        case VIR_NODE_DEV_CAP_NET:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_NET;
            break;
        case VIR_NODE_DEV_CAP_SCSI_HOST:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI_HOST;
            break;
        case VIR_NODE_DEV_CAP_SCSI_TARGET:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI_TARGET;
            break;
        case VIR_NODE_DEV_CAP_SCSI:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI;
            break;
        case VIR_NODE_DEV_CAP_STORAGE:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_STORAGE;
            break;
        case VIR_NODE_DEV_CAP_FC_HOST:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_FC_HOST;
            break;
        case VIR_NODE_DEV_CAP_VPORTS:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_VPORTS;
            break;
        case VIR_NODE_DEV_CAP_SCSI_GENERIC:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI_GENERIC;
            break;
        case VIR_NODE_DEV_CAP_DRM:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_DRM;
            break;
        case VIR_NODE_DEV_CAP_MDEV_TYPES:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_MDEV_TYPES;
            break;
        case VIR_NODE_DEV_CAP_MDEV:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_MDEV;
            break;
        case VIR_NODE_DEV_CAP_CCW_DEV:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_CCW_DEV;
            break;
        case VIR_NODE_DEV_CAP_CSS_DEV:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_CSS_DEV;
            break;
        case VIR_NODE_DEV_CAP_VDPA:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_VDPA;
            break;
        case VIR_NODE_DEV_CAP_AP_CARD:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_AP_CARD;
            break;
        case VIR_NODE_DEV_CAP_AP_QUEUE:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_AP_QUEUE;
            break;
        case VIR_NODE_DEV_CAP_AP_MATRIX:
            flags |= VIR_CONNECT_LIST_NODE_DEVICES_CAP_AP_MATRIX;
            break;
        case VIR_NODE_DEV_CAP_LAST:
            break;
        }
    }

    if (inactive || all)
        flags |= VIR_CONNECT_LIST_NODE_DEVICES_INACTIVE;
    if (!inactive)
        flags |= VIR_CONNECT_LIST_NODE_DEVICES_ACTIVE;

    if (!(list = virshNodeDeviceListCollect(ctl, caps, ncaps, flags))) {
        ret = false;
        goto cleanup;
    }

    if (tree) {
        char **parents = g_new0(char *, list->ndevices);
        char **names = g_new0(char *, list->ndevices);
        struct virshNodeList arrays = { names, parents };

        for (i = 0; i < list->ndevices; i++)
            names[i] = g_strdup(virNodeDeviceGetName(list->devices[i]));

        for (i = 0; i < list->ndevices; i++) {
            virNodeDevicePtr dev = list->devices[i];
            if (STRNEQ(names[i], "computer")) {
                parents[i] = g_strdup(virNodeDeviceGetParent(dev));
            } else {
                parents[i] = NULL;
            }
        }

        for (i = 0; i < list->ndevices; i++) {
            if (parents[i] == NULL &&
                vshTreePrint(ctl, virshNodeListLookup, &arrays,
                             list->ndevices, i) < 0)
                ret = false;
        }

        for (i = 0; i < list->ndevices; i++)
            VIR_FREE(parents[i]);
        VIR_FREE(parents);
        for (i = 0; i < list->ndevices; i++)
            VIR_FREE(names[i]);
        VIR_FREE(names);
    } else {
        for (i = 0; i < list->ndevices; i++)
            vshPrint(ctl, "%s\n", virNodeDeviceGetName(list->devices[i]));
    }

 cleanup:
    virshNodeDeviceListFree(list);
    return ret;
}

/*
 * "nodedev-dumpxml" command
 */
static const vshCmdInfo info_node_device_dumpxml[] = {
    {.name = "help",
     .data = N_("node device details in XML")
    },
    {.name = "desc",
     .data = N_("Output the node device details as an XML dump to stdout.")
    },
    {.name = NULL}
};


static const vshCmdOptDef opts_node_device_dumpxml[] = {
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device name or wwn pair in 'wwnn,wwpn' format"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceDumpXML(vshControl *ctl, const vshCmd *cmd)
{
    virNodeDevicePtr device = NULL;
    g_autofree char *xml = NULL;
    const char *device_value = NULL;
    bool ret = false;

    if (vshCommandOptStringReq(ctl, cmd, "device", &device_value) < 0)
         return false;

    device = vshFindNodeDevice(ctl, device_value);

    if (!device)
        goto cleanup;

    if (!(xml = virNodeDeviceGetXMLDesc(device, 0)))
        goto cleanup;

    vshPrint(ctl, "%s\n", xml);

    ret = true;
 cleanup:
    if (device)
        virNodeDeviceFree(device);
    return ret;
}

/*
 * "nodedev-detach" command
 */
static const vshCmdInfo info_node_device_detach[] = {
    {.name = "help",
     .data = N_("detach node device from its device driver")
    },
    {.name = "desc",
     .data = N_("Detach node device from its device driver before assigning to a domain.")
    },
    {.name = NULL}
};


static const vshCmdOptDef opts_node_device_detach[] = {
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device key"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = "driver",
     .type = VSH_OT_STRING,
     .help = N_("pci device assignment backend driver (e.g. 'vfio' or 'kvm')")
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceDetach(vshControl *ctl, const vshCmd *cmd)
{
    const char *name = NULL;
    const char *driverName = NULL;
    virNodeDevicePtr device;
    bool ret = true;
    virshControl *priv = ctl->privData;

    if (vshCommandOptStringReq(ctl, cmd, "device", &name) < 0)
        return false;

    ignore_value(vshCommandOptStringQuiet(ctl, cmd, "driver", &driverName));

    if (!(device = virNodeDeviceLookupByName(priv->conn, name))) {
        vshError(ctl, _("Could not find matching device '%s'"), name);
        return false;
    }

    if (driverName) {
        /* we must use the newer API that accepts a driverName */
        if (virNodeDeviceDetachFlags(device, driverName, 0) < 0)
            ret = false;
    } else {
        /* Yes, our (old) public API is misspelled.  At least virsh
         * can accept either spelling.  */
        if (virNodeDeviceDettach(device) < 0)
            ret = false;
    }

    if (ret)
        vshPrintExtra(ctl, _("Device %s detached\n"), name);
    else
        vshError(ctl, _("Failed to detach device %s"), name);

    virNodeDeviceFree(device);
    return ret;
}

/*
 * "nodedev-reattach" command
 */
static const vshCmdInfo info_node_device_reattach[] = {
    {.name = "help",
     .data = N_("reattach node device to its device driver")
    },
    {.name = "desc",
     .data = N_("Reattach node device to its device driver once released by the domain.")
    },
    {.name = NULL}
};


static const vshCmdOptDef opts_node_device_reattach[] = {
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device key"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceReAttach(vshControl *ctl, const vshCmd *cmd)
{
    const char *name = NULL;
    virNodeDevicePtr device;
    bool ret = true;
    virshControl *priv = ctl->privData;

    if (vshCommandOptStringReq(ctl, cmd, "device", &name) < 0)
        return false;

    if (!(device = virNodeDeviceLookupByName(priv->conn, name))) {
        vshError(ctl, _("Could not find matching device '%s'"), name);
        return false;
    }

    if (virNodeDeviceReAttach(device) == 0) {
        vshPrintExtra(ctl, _("Device %s re-attached\n"), name);
    } else {
        vshError(ctl, _("Failed to re-attach device %s"), name);
        ret = false;
    }

    virNodeDeviceFree(device);
    return ret;
}

/*
 * "nodedev-reset" command
 */
static const vshCmdInfo info_node_device_reset[] = {
    {.name = "help",
     .data = N_("reset node device")
    },
    {.name = "desc",
     .data = N_("Reset node device before or after assigning to a domain.")
    },
    {.name = NULL}
};


static const vshCmdOptDef opts_node_device_reset[] = {
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device key"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceReset(vshControl *ctl, const vshCmd *cmd)
{
    const char *name = NULL;
    virNodeDevicePtr device;
    bool ret = true;
    virshControl *priv = ctl->privData;

    if (vshCommandOptStringReq(ctl, cmd, "device", &name) < 0)
        return false;

    if (!(device = virNodeDeviceLookupByName(priv->conn, name))) {
        vshError(ctl, _("Could not find matching device '%s'"), name);
        return false;
    }

    if (virNodeDeviceReset(device) == 0) {
        vshPrintExtra(ctl, _("Device %s reset\n"), name);
    } else {
        vshError(ctl, _("Failed to reset device %s"), name);
        ret = false;
    }

    virNodeDeviceFree(device);
    return ret;
}

/*
 * "nodedev-event" command
 */
VIR_ENUM_DECL(virshNodeDeviceEvent);
VIR_ENUM_IMPL(virshNodeDeviceEvent,
              VIR_NODE_DEVICE_EVENT_LAST,
              N_("Created"),
              N_("Deleted"),
              N_("Defined"),
              N_("Undefined"));

static const char *
virshNodeDeviceEventToString(int event)
{
    const char *str = virshNodeDeviceEventTypeToString(event);
    return str ? _(str) : _("unknown");
}

struct virshNodeDeviceEventData {
    vshControl *ctl;
    bool loop;
    bool timestamp;
    int count;
    virshNodeDeviceEventCallback *cb;
};
typedef struct virshNodeDeviceEventData virshNodeDeviceEventData;

static void
vshEventLifecyclePrint(virConnectPtr conn G_GNUC_UNUSED,
                       virNodeDevicePtr dev,
                       int event,
                       int detail G_GNUC_UNUSED,
                       void *opaque)
{
    virshNodeDeviceEventData *data = opaque;

    if (!data->loop && data->count)
        return;

    if (data->timestamp) {
        char timestamp[VIR_TIME_STRING_BUFLEN];

        if (virTimeStringNowRaw(timestamp) < 0)
            timestamp[0] = '\0';

        vshPrint(data->ctl, _("%s: event 'lifecycle' for node device %s: %s\n"),
                 timestamp,
                 virNodeDeviceGetName(dev), virshNodeDeviceEventToString(event));
    } else {
        vshPrint(data->ctl, _("event 'lifecycle' for node device %s: %s\n"),
                 virNodeDeviceGetName(dev), virshNodeDeviceEventToString(event));
    }

    data->count++;
    if (!data->loop)
        vshEventDone(data->ctl);
}

static void
vshEventGenericPrint(virConnectPtr conn G_GNUC_UNUSED,
                     virNodeDevicePtr dev,
                     void *opaque)
{
    virshNodeDeviceEventData *data = opaque;

    if (!data->loop && data->count)
        return;

    if (data->timestamp) {
        char timestamp[VIR_TIME_STRING_BUFLEN];

        if (virTimeStringNowRaw(timestamp) < 0)
            timestamp[0] = '\0';

        vshPrint(data->ctl, _("%s: event '%s' for node device %s\n"),
                 timestamp,
                 data->cb->name,
                 virNodeDeviceGetName(dev));
    } else {
        vshPrint(data->ctl, _("event '%s' for node device %s\n"),
                 data->cb->name,
                 virNodeDeviceGetName(dev));
    }

    data->count++;
    if (!data->loop)
        vshEventDone(data->ctl);
}

virshNodeDeviceEventCallback virshNodeDeviceEventCallbacks[] = {
    { "lifecycle",
      VIR_NODE_DEVICE_EVENT_CALLBACK(vshEventLifecyclePrint), },
    { "update", vshEventGenericPrint, }
};
G_STATIC_ASSERT(VIR_NODE_DEVICE_EVENT_ID_LAST == G_N_ELEMENTS(virshNodeDeviceEventCallbacks));


static const vshCmdInfo info_node_device_event[] = {
    {.name = "help",
     .data = N_("Node Device Events")
    },
    {.name = "desc",
     .data = N_("List event types, or wait for node device events to occur")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_device_event[] = {
    {.name = "device",
     .type = VSH_OT_STRING,
     .help = N_("filter by node device name"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = "event",
     .type = VSH_OT_STRING,
     .completer = virshNodeDeviceEventNameCompleter,
     .help = N_("which event type to wait for")
    },
    {.name = "loop",
     .type = VSH_OT_BOOL,
     .help = N_("loop until timeout or interrupt, rather than one-shot")
    },
    {.name = "timeout",
     .type = VSH_OT_INT,
     .help = N_("timeout seconds")
    },
    {.name = "list",
     .type = VSH_OT_BOOL,
     .help = N_("list valid event types")
    },
    {.name = "timestamp",
     .type = VSH_OT_BOOL,
     .help = N_("show timestamp for each printed event")
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceEvent(vshControl *ctl, const vshCmd *cmd)
{
    virNodeDevicePtr dev = NULL;
    bool ret = false;
    int eventId = -1;
    int timeout = 0;
    virshNodeDeviceEventData data;
    const char *eventName = NULL;
    const char *device_value = NULL;
    int event;
    virshControl *priv = ctl->privData;

    if (vshCommandOptBool(cmd, "list")) {
        size_t i;

        for (i = 0; i < VIR_NODE_DEVICE_EVENT_ID_LAST; i++)
            vshPrint(ctl, "%s\n", virshNodeDeviceEventCallbacks[i].name);
        return true;
    }

    if (vshCommandOptStringReq(ctl, cmd, "event", &eventName) < 0)
        return false;
    if (!eventName) {
        vshError(ctl, "%s", _("either --list or --event <type> is required"));
        return false;
    }

    for (event = 0; event < VIR_NODE_DEVICE_EVENT_ID_LAST; event++)
        if (STREQ(eventName, virshNodeDeviceEventCallbacks[event].name))
            break;
    if (event == VIR_NODE_DEVICE_EVENT_ID_LAST) {
        vshError(ctl, _("unknown event type %s"), eventName);
        return false;
    }

    data.ctl = ctl;
    data.loop = vshCommandOptBool(cmd, "loop");
    data.timestamp = vshCommandOptBool(cmd, "timestamp");
    data.count = 0;
    data.cb = &virshNodeDeviceEventCallbacks[event];
    if (vshCommandOptTimeoutToMs(ctl, cmd, &timeout) < 0)
        return false;
    if (vshCommandOptStringReq(ctl, cmd, "device", &device_value) < 0)
        return false;

    if (device_value) {
        if (!(dev = virNodeDeviceLookupByName(priv->conn, device_value))) {
            vshError(ctl, "%s '%s'",
                     _("Could not find matching device"), device_value);
            goto cleanup;
        }
    }
    if (vshEventStart(ctl, timeout) < 0)
        goto cleanup;

    if ((eventId = virConnectNodeDeviceEventRegisterAny(priv->conn, dev, event,
                                                     data.cb->cb,
                                                     &data, NULL)) < 0)
        goto cleanup;
    switch (vshEventWait(ctl)) {
    case VSH_EVENT_INTERRUPT:
        vshPrint(ctl, "%s", _("event loop interrupted\n"));
        break;
    case VSH_EVENT_TIMEOUT:
        vshPrint(ctl, "%s", _("event loop timed out\n"));
        break;
    case VSH_EVENT_DONE:
        break;
    default:
        goto cleanup;
    }
    vshPrint(ctl, _("events received: %d\n"), data.count);
    if (data.count)
        ret = true;

 cleanup:
    vshEventCleanup(ctl);
    if (eventId >= 0 &&
        virConnectNodeDeviceEventDeregisterAny(priv->conn, eventId) < 0)
        ret = false;
    if (dev)
        virNodeDeviceFree(dev);
    return ret;
}


/*
 * "nodedev-undefine" command
 */
static const vshCmdInfo info_node_device_undefine[] = {
    {.name = "help",
     .data = N_("Undefine an inactive node device")
    },
    {.name = "desc",
     .data = N_("Undefines the configuration for an inactive node device")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_device_undefine[] = {
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device name or wwn pair in 'wwnn,wwpn' format"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceUndefine(vshControl *ctl, const vshCmd *cmd G_GNUC_UNUSED)
{
    virNodeDevice *dev = NULL;
    bool ret = false;
    const char *device_value = NULL;

    if (vshCommandOptStringReq(ctl, cmd, "device", &device_value) < 0)
        return false;

    dev = vshFindNodeDevice(ctl, device_value);

    if (!dev)
        goto cleanup;

    if (virNodeDeviceUndefine(dev, 0) == 0) {
        vshPrintExtra(ctl, _("Undefined node device '%s'\n"), device_value);
    } else {
        vshError(ctl, _("Failed to undefine node device '%s'"), device_value);
        goto cleanup;
    }

    ret = true;
 cleanup:
    if (dev)
        virNodeDeviceFree(dev);
    return ret;
}


/*
 * "nodedev-define" command
 */
static const vshCmdInfo info_node_device_define[] = {
    {.name = "help",
     .data = N_("Define a device by an xml file on a node")
    },
    {.name = "desc",
     .data = N_("Defines a persistent device on the node that can be "
                "assigned to a domain. The device must be started before "
                "it can be assigned to a domain.")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_device_define[] = {
    VIRSH_COMMON_OPT_FILE(N_("file containing an XML description "
                             "of the device")),
    {.name = NULL}
};

static bool
cmdNodeDeviceDefine(vshControl *ctl, const vshCmd *cmd G_GNUC_UNUSED)
{
    virNodeDevice *dev = NULL;
    const char *from = NULL;
    bool ret = true;
    g_autofree char *buffer = NULL;
    virshControl *priv = ctl->privData;

    if (vshCommandOptStringReq(ctl, cmd, "file", &from) < 0)
        return false;

    if (virFileReadAll(from, VSH_MAX_XML_FILE, &buffer) < 0)
        return false;

    dev = virNodeDeviceDefineXML(priv->conn, buffer, 0);

    if (dev != NULL) {
        vshPrintExtra(ctl, _("Node device '%s' defined from '%s'\n"),
                      virNodeDeviceGetName(dev), from);
        virNodeDeviceFree(dev);
    } else {
        vshError(ctl, _("Failed to define node device from '%s'"), from);
        ret = false;
    }

    return ret;
}


/*
 * "nodedev-start" command
 */
static const vshCmdInfo info_node_device_start[] = {
    {.name = "help",
     .data = N_("Start an inactive node device")
    },
    {.name = "desc",
     .data = N_("Starts an inactive node device that was previously defined")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_node_device_start[] = {
    {.name = "device",
     .type = VSH_OT_DATA,
     .flags = VSH_OFLAG_REQ,
     .help = N_("device name"),
     .completer = virshNodeDeviceNameCompleter,
    },
    {.name = NULL}
};

static bool
cmdNodeDeviceStart(vshControl *ctl, const vshCmd *cmd)
{
    const char *name = NULL;
    virNodeDevice *device;
    bool ret = true;
    virshControl *priv = ctl->privData;

    if (vshCommandOptStringReq(ctl, cmd, "device", &name) < 0)
        return false;

    if (!(device = virNodeDeviceLookupByName(priv->conn, name))) {
        vshError(ctl, _("Could not find matching device '%s'"), name);
        return false;
    }

    if (virNodeDeviceCreate(device, 0) == 0) {
        vshPrintExtra(ctl, _("Device %s started\n"), name);
    } else {
        vshError(ctl, _("Failed to start device %s"), name);
        ret = false;
    }

    virNodeDeviceFree(device);
    return ret;
}


const vshCmdDef nodedevCmds[] = {
    {.name = "nodedev-create",
     .handler = cmdNodeDeviceCreate,
     .opts = opts_node_device_create,
     .info = info_node_device_create,
     .flags = 0
    },
    {.name = "nodedev-destroy",
     .handler = cmdNodeDeviceDestroy,
     .opts = opts_node_device_destroy,
     .info = info_node_device_destroy,
     .flags = 0
    },
    {.name = "nodedev-detach",
     .handler = cmdNodeDeviceDetach,
     .opts = opts_node_device_detach,
     .info = info_node_device_detach,
     .flags = 0
    },
    {.name = "nodedev-dettach",
     .flags = VSH_CMD_FLAG_ALIAS,
     .alias = "nodedev-detach"
    },
    {.name = "nodedev-dumpxml",
     .handler = cmdNodeDeviceDumpXML,
     .opts = opts_node_device_dumpxml,
     .info = info_node_device_dumpxml,
     .flags = 0
    },
    {.name = "nodedev-list",
     .handler = cmdNodeListDevices,
     .opts = opts_node_list_devices,
     .info = info_node_list_devices,
     .flags = 0
    },
    {.name = "nodedev-reattach",
     .handler = cmdNodeDeviceReAttach,
     .opts = opts_node_device_reattach,
     .info = info_node_device_reattach,
     .flags = 0
    },
    {.name = "nodedev-reset",
     .handler = cmdNodeDeviceReset,
     .opts = opts_node_device_reset,
     .info = info_node_device_reset,
     .flags = 0
    },
    {.name = "nodedev-event",
     .handler = cmdNodeDeviceEvent,
     .opts = opts_node_device_event,
     .info = info_node_device_event,
     .flags = 0
    },
    {.name = "nodedev-define",
     .handler = cmdNodeDeviceDefine,
     .opts = opts_node_device_define,
     .info = info_node_device_define,
     .flags = 0
    },
    {.name = "nodedev-undefine",
     .handler = cmdNodeDeviceUndefine,
     .opts = opts_node_device_undefine,
     .info = info_node_device_undefine,
     .flags = 0
    },
    {.name = "nodedev-start",
     .handler = cmdNodeDeviceStart,
     .opts = opts_node_device_start,
     .info = info_node_device_start,
     .flags = 0
    },
    {.name = NULL}
};
