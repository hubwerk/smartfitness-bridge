#include "ant_usb_transport.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/message_buffer.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include <cstdio>

namespace {
constexpr std::size_t kPacketSize = 64;  // full-speed bulk max packet size
constexpr std::size_t kRxBufferSize = 1024;
constexpr std::size_t kTxBufferSize = 512;
constexpr std::uint8_t kInterface = 0;
}  // namespace

struct AntUsbTransport::Impl {
    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t device = nullptr;
    usb_transfer_t* in = nullptr;
    usb_transfer_t* out = nullptr;
    StreamBufferHandle_t rx = nullptr;
    MessageBufferHandle_t tx = nullptr;
    std::uint8_t inEndpoint = 0;
    std::uint8_t outEndpoint = 0;
    std::uint16_t inPacketSize = kPacketSize;
    bool interfaceClaimed = false;
    std::atomic<bool> ready{false};
    std::atomic<bool> inBusy{false};
    std::atomic<bool> outBusy{false};
    std::atomic<bool> gone{false};
    std::uint8_t newAddress = 0;  // pending NEW_DEV (client task only)
    char description[24] = "USB-OTG: no stick";

    static void libraryTask(void* arg);
    static void clientTask(void* arg);
    static void clientEvent(const usb_host_client_event_msg_t* message, void* arg);
    static void inDone(usb_transfer_t* transfer);
    static void outDone(usb_transfer_t* transfer);

    void open(std::uint8_t address);
    void close();
    void submitIn();
    void serviceTx();
};

void AntUsbTransport::Impl::libraryTask(void*) {
    for (;;) {
        std::uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

void AntUsbTransport::Impl::clientTask(void* arg) {
    auto* self = static_cast<Impl*>(arg);
    for (;;) {
        usb_host_client_handle_events(self->client, pdMS_TO_TICKS(5));
        if (self->newAddress != 0 && self->device == nullptr) {
            const std::uint8_t address = self->newAddress;
            self->newAddress = 0;
            self->open(address);
        }
        if (self->gone.load() && !self->inBusy.load() && !self->outBusy.load()) self->close();
        if (self->ready.load()) self->serviceTx();
    }
}

void AntUsbTransport::Impl::clientEvent(const usb_host_client_event_msg_t* message, void* arg) {
    auto* self = static_cast<Impl*>(arg);
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        self->newAddress = message->new_dev.address;
    } else if (message->event == USB_HOST_CLIENT_EVENT_DEV_GONE && message->dev_gone.dev_hdl == self->device) {
        self->ready = false;
        self->gone = true;
    }
}

void AntUsbTransport::Impl::inDone(usb_transfer_t* transfer) {
    auto* self = static_cast<Impl*>(transfer->context);
    self->inBusy = false;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0)
        xStreamBufferSend(self->rx, transfer->data_buffer, transfer->actual_num_bytes, 0);
    const bool retry = transfer->status == USB_TRANSFER_STATUS_COMPLETED ||
                       transfer->status == USB_TRANSFER_STATUS_TIMED_OUT;
    if (retry && self->ready.load()) self->submitIn();
}

void AntUsbTransport::Impl::outDone(usb_transfer_t* transfer) {
    static_cast<Impl*>(transfer->context)->outBusy = false;
}

void AntUsbTransport::Impl::submitIn() {
    in->device_handle = device;
    in->bEndpointAddress = inEndpoint;
    in->num_bytes = static_cast<int>(usb_round_up_to_mps(kPacketSize, inPacketSize));
    in->callback = inDone;
    in->context = this;
    inBusy = true;
    if (usb_host_transfer_submit(in) != ESP_OK) inBusy = false;
}

void AntUsbTransport::Impl::serviceTx() {
    if (outBusy.load()) return;
    const std::size_t length = xMessageBufferReceive(tx, out->data_buffer, kPacketSize, 0);
    if (length == 0) return;
    out->device_handle = device;
    out->bEndpointAddress = outEndpoint;
    out->num_bytes = static_cast<int>(length);
    out->callback = outDone;
    out->context = this;
    outBusy = true;
    if (usb_host_transfer_submit(out) != ESP_OK) outBusy = false;
}

void AntUsbTransport::Impl::open(std::uint8_t address) {
    if (usb_host_device_open(client, address, &device) != ESP_OK) {
        device = nullptr;
        return;
    }
    const usb_device_desc_t* deviceDesc = nullptr;
    const usb_config_desc_t* configDesc = nullptr;
    if (usb_host_get_device_descriptor(device, &deviceDesc) != ESP_OK ||
        !AntUsbTransport::supported(deviceDesc->idVendor, deviceDesc->idProduct) ||
        usb_host_get_active_config_descriptor(device, &configDesc) != ESP_OK) {
        usb_host_device_close(client, device);
        device = nullptr;
        return;
    }
    int offset = 0;
    const usb_intf_desc_t* interface = usb_parse_interface_descriptor(configDesc, kInterface, 0, &offset);
    inEndpoint = outEndpoint = 0;
    for (int index = 0; interface && index < interface->bNumEndpoints; ++index) {
        int endpointOffset = offset;
        const usb_ep_desc_t* endpoint =
            usb_parse_endpoint_descriptor_by_index(interface, index, configDesc->wTotalLength, &endpointOffset);
        if (!endpoint || (endpoint->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
            continue;
        if (endpoint->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
            inEndpoint = endpoint->bEndpointAddress;
            inPacketSize = endpoint->wMaxPacketSize ? endpoint->wMaxPacketSize : kPacketSize;
        } else {
            outEndpoint = endpoint->bEndpointAddress;
        }
    }
    if (!inEndpoint || !outEndpoint || usb_host_interface_claim(client, device, kInterface, 0) != ESP_OK) {
        usb_host_device_close(client, device);
        device = nullptr;
        return;
    }
    interfaceClaimed = true;
    std::snprintf(description, sizeof(description), "USB %04x:%04x", deviceDesc->idVendor, deviceDesc->idProduct);
    xStreamBufferReset(rx);
    xMessageBufferReset(tx);
    gone = false;
    ready = true;
    submitIn();
    Serial.printf("[ANT] USB stick %s attached\n", description);
}

void AntUsbTransport::Impl::close() {
    ready = false;
    gone = false;
    if (device) {
        if (interfaceClaimed) usb_host_interface_release(client, device, kInterface);
        usb_host_device_close(client, device);
    }
    interfaceClaimed = false;
    device = nullptr;
    std::snprintf(description, sizeof(description), "USB-OTG: no stick");
    Serial.println("[ANT] USB stick removed");
}

bool AntUsbTransport::begin() {
    if (impl_) return true;
    if (failed_) return false;
    auto* impl = new Impl;
    impl->rx = xStreamBufferCreate(kRxBufferSize, 1);
    impl->tx = xMessageBufferCreate(kTxBufferSize);
    const usb_host_config_t hostConfig = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    usb_host_client_config_t clientConfig = {};
    clientConfig.is_synchronous = false;
    clientConfig.max_num_event_msg = 5;
    clientConfig.async.client_event_callback = Impl::clientEvent;
    clientConfig.async.callback_arg = impl;
    if (!impl->rx || !impl->tx || usb_host_install(&hostConfig) != ESP_OK) {
        failed_ = true;
        Serial.println("[ANT] USB host could not be started");
        return false;
    }
    if (usb_host_client_register(&clientConfig, &impl->client) != ESP_OK ||
        usb_host_transfer_alloc(kPacketSize, 0, &impl->in) != ESP_OK ||
        usb_host_transfer_alloc(kPacketSize, 0, &impl->out) != ESP_OK) {
        failed_ = true;
        Serial.println("[ANT] USB host client could not be registered");
        return false;
    }
    xTaskCreatePinnedToCore(Impl::libraryTask, "usb_host", 4096, impl, 2, nullptr, 0);
    xTaskCreatePinnedToCore(Impl::clientTask, "ant_usb", 4096, impl, 2, nullptr, 0);
    impl_ = impl;
    Serial.println("[ANT] USB host started; waiting for an ANT+ USB stick on the USB-OTG port");
    return true;
}

bool AntUsbTransport::connected() const { return impl_ && impl_->ready.load(); }

bool AntUsbTransport::write(const std::uint8_t* data, std::size_t length) {
    if (!connected() || length == 0 || length > kPacketSize) return false;
    return xMessageBufferSend(impl_->tx, data, length, 0) == length;
}

std::size_t AntUsbTransport::read(std::uint8_t* data, std::size_t capacity) {
    if (!impl_) return 0;
    return xStreamBufferReceive(impl_->rx, data, capacity, 0);
}

const char* AntUsbTransport::description() const { return impl_ ? impl_->description : "USB-OTG: not started"; }

#endif  // ARDUINO
