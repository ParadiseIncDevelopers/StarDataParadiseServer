using System;
using System.Diagnostics;
using System.IO;
using System.Web.Script.Serialization;
using System.Collections.Generic;
using System.Reflection;

[assembly: AssemblyTitle("StarDataParadise AI Worker Launcher")]
[assembly: AssemblyVersion("0.5.2.0")]
[assembly: AssemblyFileVersion("0.5.2.0")]

internal static class WorkerLauncher {
    private static string Quote(string value) {
        if (value.IndexOf('"') >= 0 || value.IndexOf('\0') >= 0) throw new ArgumentException("Invalid deployment path.");
        return "\"" + value.TrimEnd('\\', '/') + "\"";
    }
    [STAThread]
    private static int Main(string[] args) {
        string directory = AppDomain.CurrentDomain.BaseDirectory;
        try {
            if (args.Length == 2 && args[0] == "--deployment") directory = Path.GetFullPath(args[1]);
            else if (args.Length != 0) throw new ArgumentException("Use --deployment <installed directory>.");
            var json = new JavaScriptSerializer();
            var details = json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(directory, "connection.json")));
            string cli = Path.GetFullPath((string)details["mysqlsh_executable"]);
            string entry = Path.Combine(directory, "runtime", "compatibility", "ai", "Run-LocalWorker.py");
            if (!File.Exists(cli) || !File.Exists(entry) || !String.Equals(Path.GetFileName(cli), "mysqlsh.exe", StringComparison.OrdinalIgnoreCase))
                throw new FileNotFoundException("Installed MySQL Shell or worker runtime is missing.");
            string stop = Path.Combine(directory, "stop");
            if (File.Exists(stop)) File.Delete(stop);
            var start = new ProcessStartInfo(cli, "--py --disable-plugins --log-level=error --file=" + Quote(entry) + " " + Quote(directory));
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.WorkingDirectory = directory;
            // A Workbench plugin runs inside its own embedded Python. Its
            // PYTHONHOME/PYTHONPATH must not select that Python for mysqlsh.
            var inheritedPython = new List<string>();
            foreach (string key in start.EnvironmentVariables.Keys)
                if (key.StartsWith("PYTHON", StringComparison.OrdinalIgnoreCase)) inheritedPython.Add(key);
            foreach (string key in inheritedPython) start.EnvironmentVariables.Remove(key);
            start.EnvironmentVariables["MYSQLSH_USER_CONFIG_HOME"] = Path.Combine(directory, "cli-profile");
            Directory.CreateDirectory(start.EnvironmentVariables["MYSQLSH_USER_CONFIG_HOME"]);
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            using (var process = new Process()) {
                process.StartInfo = start;
                object gate = new object();
                DataReceivedEventHandler log = delegate(object sender, DataReceivedEventArgs item) {
                    // The worker protocol prints status identifiers only. Do not
                    // print the environment, connection options or credentials.
                    if (item.Data != null) lock (gate) File.AppendAllText(Path.Combine(directory, "launcher.log"), item.Data + Environment.NewLine);
                };
                process.OutputDataReceived += log;
                process.ErrorDataReceived += log;
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                process.WaitForExit();
                return process.ExitCode;
            }
        } catch (Exception error) {
            try { File.AppendAllText(Path.Combine(directory, "launcher-error.log"), DateTime.UtcNow.ToString("o") + " " + error.GetType().Name + Environment.NewLine); } catch { }
            return 1;
        }
    }
}
