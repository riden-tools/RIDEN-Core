package io.riden.core;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;
import java.io.IOException;

/** Call only after the host application obtains UsbManager user permission. */
public final class UsbTransport implements RidenCore.Transport {
    private final UsbSerialPort port;
    private final UsbDeviceConnection connection;
    public UsbTransport(UsbManager manager, UsbDevice device) throws IOException {
        if (device.getVendorId() != 0x1a86 || device.getProductId() != 0x7523)
            throw new IOException("Expected CH340 1a86:7523");
        if (!manager.hasPermission(device)) throw new IOException("USB permission required");
        UsbSerialDriver driver = UsbSerialProber.getDefaultProber().probeDevice(device);
        if (driver == null) throw new IOException("No serial driver");
        connection = manager.openDevice(device);
        if (connection == null) throw new IOException("Cannot open USB device");
        port = driver.getPorts().get(0);
        try {
            port.open(connection);
            port.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE);
            port.setDTR(true); port.setRTS(true); port.purgeHwBuffers(true, true);
        } catch (IOException | RuntimeException e) {
            try { port.close(); } catch (IOException ignored) { }
            connection.close(); throw e;
        }
    }
    public int read(byte[] buffer, int timeoutMs) throws IOException { return port.read(buffer, timeoutMs); }
    public int write(byte[] data, int timeoutMs) throws IOException { port.write(data, timeoutMs); return data.length; }
    public void close() throws IOException { try { port.close(); } finally { connection.close(); } }
}
