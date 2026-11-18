// Pairs this PC with a Bluetooth LE HID device by advertised name, with no question on screen
// (same result as Settings > Bluetooth > Add device). Build and run: bt\pair.ps1 [-Name Kindle] [-Unpair]
using System;
using System.Threading;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Enumeration;
using Windows.Foundation;

public static class Pair {
	static T Wait<T>(IAsyncOperation<T> op) {
		for (int i = 0; i < 900 && op.Status == AsyncStatus.Started; i++) Thread.Sleep(100);
		if (op.Status != AsyncStatus.Completed) throw new Exception("operation " + op.Status + ": " + op.ErrorCode);
		return op.GetResults();
	}

	public static int Main(string[] args) {
		string name = args.Length > 0 ? args[0] : "Kindle";
		bool unpair = args.Length > 1 && args[1] == "unpair";
		ulong addr = 0;
		var w = new BluetoothLEAdvertisementWatcher();
		w.ScanningMode = BluetoothLEScanningMode.Active;
		w.Received += (s, a) => {
			bool hid = false;
			foreach (var u in a.Advertisement.ServiceUuids) if (u.ToString().StartsWith("00001812")) hid = true;
			if (hid && a.Advertisement.LocalName == name) addr = a.BluetoothAddress;
		};
		w.Start();
		for (int i = 0; i < 150 && addr == 0; i++) Thread.Sleep(100);
		w.Stop();
		if (addr == 0) { Console.WriteLine("no HID device named {0} is advertising", name); return 1; }

		var dev = Wait(BluetoothLEDevice.FromBluetoothAddressAsync(addr));
		var p = dev.DeviceInformation.Pairing;
		Console.WriteLine("{0:X12} \"{1}\" paired={2} canPair={3}", addr, dev.Name, p.IsPaired, p.CanPair);
		if (unpair) { Console.WriteLine("unpair: {0}", Wait(p.UnpairAsync()).Status); return 0; }
		if (p.IsPaired) return 0;
		p.Custom.PairingRequested += (s, a) => { Console.WriteLine("pairing kind {0}, pin \"{1}\": accepted", a.PairingKind, a.Pin); a.Accept(); };
		var r = Wait(p.Custom.PairAsync(DevicePairingKinds.ConfirmOnly | DevicePairingKinds.DisplayPin | DevicePairingKinds.ConfirmPinMatch,
			DevicePairingProtectionLevel.None));
		Console.WriteLine("pair: {0}, protection {1}", r.Status, r.ProtectionLevelUsed);
		return r.Status == DevicePairingResultStatus.Paired ? 0 : 1;
	}
}
