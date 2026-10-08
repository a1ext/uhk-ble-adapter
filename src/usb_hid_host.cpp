#include "usb_hid_host.h"
#include <Arduino.h>
#include <usb/usb_host.h>
#include "log.h"

static const uint8_t  DESC_TYPE_INTERFACE = 0x04;
static const uint8_t  DESC_TYPE_ENDPOINT  = 0x05;
static const uint8_t  DESC_TYPE_HID       = 0x21;
static const uint8_t  DESC_TYPE_REPORT    = 0x22;
static const uint8_t  HID_SET_REPORT      = 0x09;
static const uint8_t  HID_SET_IDLE        = 0x0A;
static const int      MAX_IFACES          = 8;
static const size_t   CTRL_BUF_SIZE       = 1024;
static const uint32_t CTRL_WARN_MS        = 2000;

struct SetReportReq {
    uint8_t  ifaceIdx, type, id;
    uint16_t len;
    uint8_t  data[64];
};

struct InPipe {
    usb_transfer_t *xfer;
    uint8_t ifaceIdx;
    volatile bool inFlight;
    volatile bool held;       // waiting for room in the consumer (or after an error)
    volatile bool needClear;  // endpoint halted by a transfer error
};

static UsbHidHostCallbacks cbs;
static bool started = false;
static usb_host_client_handle_t client;
static QueueHandle_t setReportQueue;

// Client task state
static volatile uint8_t newDevAddr = 0; // set by the client event callback
static volatile bool    devGone = false;
static usb_device_handle_t dev = nullptr;
static UsbHidDevice devInfo;
static bool    claimed[MAX_IFACES];
static InPipe  pipes[MAX_IFACES];
static int     numPipes = 0;
static bool    polling = false;
static bool    readyReported = false;
static uint32_t transferErrors = 0;

static usb_transfer_t *ctrlXfer;
static volatile bool  ctrlDone;
static usb_transfer_status_t ctrlStatus;
static int            ctrlActual;

static void clientEventCb(const usb_host_client_event_msg_t *msg, void *)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        newDevAddr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
        devGone = true;
}

static void ctrlCb(usb_transfer_t *t)
{
    ctrlStatus = t->status;
    ctrlActual = t->actual_num_bytes;
    ctrlDone = true;
}

// Synchronous control transfer, pumping client events while waiting (the
// completion callback runs from usb_host_client_handle_events). Returns the
// number of data bytes transferred, or -1 on failure.
static int ctrlSync(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                    uint16_t wIndex, uint16_t wLength, const uint8_t *outData = nullptr)
{
    if (!dev)
        return -1;
    if (wLength > CTRL_BUF_SIZE - sizeof(usb_setup_packet_t))
        wLength = CTRL_BUF_SIZE - sizeof(usb_setup_packet_t);

    auto *setup = (usb_setup_packet_t *)ctrlXfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;
    if (outData && wLength)
        memcpy(ctrlXfer->data_buffer + sizeof(usb_setup_packet_t), outData, wLength);

    ctrlXfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    ctrlXfer->device_handle = dev;
    ctrlXfer->bEndpointAddress = 0;
    ctrlXfer->callback = ctrlCb;
    ctrlXfer->context = nullptr;
    ctrlXfer->timeout_ms = 1000;
    ctrlDone = false;
    if (usb_host_transfer_submit_control(client, ctrlXfer) != ESP_OK)
        return -1;

    // A gone device completes the transfer with NO_DEVICE, so this terminates
    uint32_t t0 = millis();
    bool warned = false;
    while (!ctrlDone) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
        if (!warned && millis() - t0 > CTRL_WARN_MS) {
            warned = true;
            logf("USB: control request %02X/%02X is not answered", bmRequestType, bRequest);
        }
    }
    if (ctrlStatus != USB_TRANSFER_STATUS_COMPLETED)
        return -1;
    return ctrlActual - (int)sizeof(usb_setup_packet_t);
}

static std::string strDesc(const usb_str_desc_t *d)
{
    std::string s;
    if (!d)
        return s;
    for (int i = 0; i < (d->bLength - 2) / 2; i++) {
        uint16_t c = d->wData[i];
        s += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    return s;
}

// HID interfaces (alternate setting 0) with their report descriptor length
// and first interrupt IN endpoint
static void parseConfig(const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    int total = cfg->wTotalLength;
    int cur = -1;
    for (int off = 0; off + 2 <= total;) {
        uint8_t len = p[off], type = p[off + 1];
        if (len < 2 || off + len > total)
            break;
        if (type == DESC_TYPE_INTERFACE && len >= 9) {
            cur = -1;
            if (p[off + 5] == USB_CLASS_HID && p[off + 3] == 0 && devInfo.ifaces.size() < MAX_IFACES) {
                UsbHidIface i{};
                i.num = p[off + 2];
                i.protocol = p[off + 7];
                devInfo.ifaces.push_back(i);
                cur = devInfo.ifaces.size() - 1;
            }
        } else if (type == DESC_TYPE_HID && cur >= 0 && len >= 9) {
            uint8_t n = p[off + 5];
            for (uint8_t k = 0; k < n && off + 8 + 3 * k < off + len; k++) {
                if (p[off + 6 + 3 * k] == DESC_TYPE_REPORT)
                    devInfo.ifaces[cur].reportDescLen = p[off + 7 + 3 * k] | (p[off + 8 + 3 * k] << 8);
            }
        } else if (type == DESC_TYPE_ENDPOINT && cur >= 0 && len >= 7) {
            uint8_t addr = p[off + 2], attr = p[off + 3];
            UsbHidIface &i = devInfo.ifaces[cur];
            if ((addr & 0x80) && (attr & 0x03) == 0x03 && !i.epIn) {
                i.epIn = addr;
                i.mps = (p[off + 4] | (p[off + 5] << 8)) & 0x7FF;
                i.interval = p[off + 6];
            }
        }
        off += len;
    }
}

static void inCb(usb_transfer_t *t);

static void submitIn(InPipe &p)
{
    p.held = false;
    if (usb_host_transfer_submit(p.xfer) == ESP_OK)
        p.inFlight = true;
    else
        p.held = true; // retried from the client loop
}

static void inCb(usb_transfer_t *t)
{
    InPipe &p = *(InPipe *)t->context;
    p.inFlight = false;

    switch (t->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        if (t->actual_num_bytes > 0 && polling && cbs.onInputReport)
            cbs.onInputReport(p.ifaceIdx, t->data_buffer, t->actual_num_bytes);
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        return; // device gone / pipe flushed
    default:
        if (transferErrors++ < 10)
            logf("USB: IN 0x%02X transfer status %d", t->bEndpointAddress, t->status);
        p.needClear = true;
        p.held = true;
        return;
    }

    if (!polling || devGone)
        return;
    if (cbs.canAcceptReport && !cbs.canAcceptReport())
        p.held = true;
    else
        submitIn(p);
}

static void openDevice(uint8_t addr)
{
    if (usb_host_device_open(client, addr, &dev) != ESP_OK) {
        logf("USB: cannot open device %u", addr);
        dev = nullptr;
        return;
    }
    devInfo = UsbHidDevice();
    memset(claimed, 0, sizeof(claimed));
    numPipes = 0;
    transferErrors = 0;

    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(dev, &dd);
    usb_device_info_t info;
    usb_host_device_info(dev, &info);
    devInfo.vid = dd->idVendor;
    devInfo.pid = dd->idProduct;
    devInfo.product = strDesc(info.str_desc_product);
    logf("USB: %04X:%04X \"%s\", %s speed", devInfo.vid, devInfo.pid, devInfo.product.c_str(),
         info.speed == USB_SPEED_FULL ? "full" : "low");

    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        logf("USB: no configuration descriptor");
        return;
    }
    parseConfig(cfg);
    if (devInfo.ifaces.empty()) {
        logf("USB: no HID interfaces - not a keyboard?");
        return;
    }

    for (size_t i = 0; i < devInfo.ifaces.size(); i++) {
        UsbHidIface &f = devInfo.ifaces[i];
        claimed[i] = usb_host_interface_claim(client, dev, f.num, 0) == ESP_OK;
        if (!claimed[i]) {
            logf("USB: IF%u claim failed", f.num);
            continue;
        }
        // GET_DESCRIPTOR(Report) is a standard request addressed to the interface
        int n = ctrlSync(0x81, 0x06, DESC_TYPE_REPORT << 8, f.num, f.reportDescLen);
        if (devGone)
            return;
        if (n > 0) {
            const uint8_t *d = ctrlXfer->data_buffer + sizeof(usb_setup_packet_t);
            f.reportDesc.assign(d, d + n);
        }
        logf("USB: IF%u EP 0x%02X mps %u int %u ms, report descriptor %d bytes",
             f.num, f.epIn, f.mps, f.interval, n);
        if (n > 0)
            log_hex("  ", f.reportDesc.data(), f.reportDesc.size());
    }
    // Keyboard: report only on change (no idle repeats). Sent after all
    // descriptors are read, so a STALL cannot get in their way.
    for (size_t i = 0; i < devInfo.ifaces.size(); i++) {
        if (claimed[i] && devInfo.ifaces[i].protocol == 1)
            ctrlSync(0x21, HID_SET_IDLE, 0, devInfo.ifaces[i].num, 0);
        if (devGone)
            return;
    }

    uint32_t mask = cbs.onDeviceReady ? cbs.onDeviceReady(devInfo) : 0;
    readyReported = true;

    numPipes = 0;
    for (size_t i = 0; i < devInfo.ifaces.size(); i++) {
        UsbHidIface &f = devInfo.ifaces[i];
        if (!(mask & (1u << i)) || !claimed[i] || !f.epIn || !f.mps)
            continue;
        InPipe &p = pipes[numPipes];
        if (usb_host_transfer_alloc(f.mps, 0, &p.xfer) != ESP_OK)
            continue;
        p.ifaceIdx = i;
        p.inFlight = p.held = p.needClear = false;
        p.xfer->num_bytes = f.mps; // IN transfers must be a multiple of MPS
        p.xfer->device_handle = dev;
        p.xfer->bEndpointAddress = f.epIn;
        p.xfer->callback = inCb;
        p.xfer->context = &p;
        p.xfer->timeout_ms = 0;
        numPipes++;
    }
    polling = true;
    for (int i = 0; i < numPipes; i++)
        submitIn(pipes[i]);
    logf("USB: polling %d interface(s)", numPipes);
}

static void closeDevice()
{
    polling = false;
    for (int i = 0; i < numPipes; i++) {
        if (pipes[i].inFlight) {
            usb_host_endpoint_halt(dev, pipes[i].xfer->bEndpointAddress);
            usb_host_endpoint_flush(dev, pipes[i].xfer->bEndpointAddress);
        }
    }
    // Flushed / failed transfers complete through the client event handler
    for (int tries = 0; tries < 50; tries++) {
        bool busy = false;
        for (int i = 0; i < numPipes; i++)
            busy |= pipes[i].inFlight;
        if (!busy)
            break;
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < numPipes; i++)
        usb_host_transfer_free(pipes[i].xfer);
    numPipes = 0;
    for (size_t i = 0; i < devInfo.ifaces.size(); i++)
        if (claimed[i])
            usb_host_interface_release(client, dev, devInfo.ifaces[i].num);
    usb_host_device_close(client, dev);
    dev = nullptr;
    xQueueReset(setReportQueue);

    if (readyReported && cbs.onDeviceGone)
        cbs.onDeviceGone();
    readyReported = false;
}

static void doSetReport(const SetReportReq &r)
{
    if (r.ifaceIdx >= devInfo.ifaces.size())
        return;
    uint8_t buf[sizeof(r.data) + 1];
    uint16_t n = 0;
    if (r.id)
        buf[n++] = r.id; // interfaces with report IDs expect the ID as the first byte
    memcpy(buf + n, r.data, r.len);
    n += r.len;
    int res = ctrlSync(0x21, HID_SET_REPORT, (r.type << 8) | r.id, devInfo.ifaces[r.ifaceIdx].num, n, buf);
    if (res < 0 && !devGone)
        logf("USB: SET_REPORT IF%u type %u id %u failed", devInfo.ifaces[r.ifaceIdx].num, r.type, r.id);
}

static void libTask(void *)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

static void clientTask(void *)
{
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 5;
    cfg.async.client_event_callback = clientEventCb;
    cfg.async.callback_arg = nullptr;
    if (usb_host_client_register(&cfg, &client) != ESP_OK ||
        usb_host_transfer_alloc(CTRL_BUF_SIZE, 0, &ctrlXfer) != ESP_OK) {
        logf("USB: client registration failed");
        vTaskDelete(nullptr);
    }

    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(5));

        if (devGone) {
            if (dev) {
                logf("USB: keyboard disconnected");
                closeDevice();
            }
            devGone = false;
        }
        if (newDevAddr && !dev) {
            uint8_t addr = newDevAddr;
            newDevAddr = 0;
            openDevice(addr);
        }
        if (!dev || !polling)
            continue;

        for (int i = 0; i < numPipes; i++) {
            InPipe &p = pipes[i];
            if (!p.held || p.inFlight)
                continue;
            if (p.needClear) {
                usb_host_endpoint_flush(dev, p.xfer->bEndpointAddress);
                usb_host_endpoint_clear(dev, p.xfer->bEndpointAddress);
                p.needClear = false;
                vTaskDelay(pdMS_TO_TICKS(10)); // do not spin on a failing endpoint
            }
            if (!cbs.canAcceptReport || cbs.canAcceptReport())
                submitIn(p);
        }

        SetReportReq req;
        while (dev && !devGone && xQueueReceive(setReportQueue, &req, 0) == pdTRUE)
            doSetReport(req);
    }
}

void usb_hid_host_begin(const UsbHidHostCallbacks &cb)
{
    if (started)
        return;
    cbs = cb;

    // The internal PHY switches from USB-Serial-JTAG to the OTG controller:
    // the native USB CDC console and USB flashing are gone until reset
    log_disable_usb_cdc();

    usb_host_config_t hc = {};
    hc.skip_phy_setup = false;
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&hc);
    if (err != ESP_OK) {
        logf("USB: host install failed: %s", esp_err_to_name(err));
        return;
    }
    setReportQueue = xQueueCreate(8, sizeof(SetReportReq));
    xTaskCreatePinnedToCore(libTask, "usb_lib", 4096, nullptr, 6, nullptr, 1);
    xTaskCreatePinnedToCore(clientTask, "usb_hid", 8192, nullptr, 5, nullptr, 1);
    started = true;
    logf("USB: host started, waiting for the keyboard");
}

bool usb_hid_host_started()
{
    return started;
}

bool usb_hid_host_set_report(uint8_t ifaceIdx, uint8_t reportType, uint8_t reportId,
                             const uint8_t *payload, uint16_t len)
{
    if (!started)
        return false;
    SetReportReq r;
    r.ifaceIdx = ifaceIdx;
    r.type = reportType;
    r.id = reportId;
    r.len = min<uint16_t>(len, sizeof(r.data));
    memcpy(r.data, payload, r.len);
    return xQueueSend(setReportQueue, &r, 0) == pdTRUE;
}
