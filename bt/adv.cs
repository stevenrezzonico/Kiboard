// Prints the raw Bluetooth LE advertisements of devices that advertise the HID service (0x1812) or carry
// a given name. Read-only. Build and run: bt\adv.ps1 [-Seconds 10] [-Name Kindle]
using System;
using System.Collections.Generic;
using Windows.Devices.Bluetooth.Advertisement;

public static class AdvScan {
	public static void Main(string[] args) {
		int seconds = args.Length > 0 ? int.Parse(args[0]) : 10;
		string name = args.Length > 1 ? args[1] : "Kindle";
		var seen = new Dictionary<string, string>();
		var w = new BluetoothLEAdvertisementWatcher();
		w.ScanningMode = BluetoothLEScanningMode.Active;
		w.Received += (s, a) => {
			bool hid = false;
			foreach (var u in a.Advertisement.ServiceUuids) if (u.ToString().StartsWith("00001812")) hid = true;
			if (!hid && a.Advertisement.LocalName != name) return;
			string line = string.Format("{0:X12} {1} rssi {2} name \"{3}\" flags {4}:", a.BluetoothAddress, a.AdvertisementType,
				a.RawSignalStrengthInDBm, a.Advertisement.LocalName, a.Advertisement.Flags);
			foreach (var d in a.Advertisement.DataSections) {
				var bytes = new byte[d.Data.Length];
				Windows.Storage.Streams.DataReader.FromBuffer(d.Data).ReadBytes(bytes);
				line += string.Format(" [{0:X2} {1}]", d.DataType, BitConverter.ToString(bytes));
			}
			lock (seen) seen[a.BluetoothAddress + a.AdvertisementType.ToString()] = line;
		};
		w.Start();
		System.Threading.Thread.Sleep(seconds * 1000);
		w.Stop();
		lock (seen) foreach (var l in seen.Values) Console.WriteLine(l);
		Console.WriteLine("{0} matching advertisements", seen.Count);
	}
}
