/* Copyright (c) 2019, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

package eu.chainfire.injectvm;

import androidx.appcompat.app.AppCompatActivity;
import eu.chainfire.injectvm.injected.IInjectedComms;
import eu.chainfire.injectvm.injected.InjectedMain;
import eu.chainfire.libsuperuser.Shell;

import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.AsyncTask;
import android.os.Bundle;
import android.os.RemoteException;
import android.text.method.ScrollingMovementMethod;
import android.view.View;
import android.widget.TextView;

import java.io.File;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class MainActivity extends AppCompatActivity {
    private static final String UUID = java.util.UUID.randomUUID().toString();

    // Receives a Binder object from the injected process for command-and-control
    private BroadcastReceiver commReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (InjectedMain.INTENT_COMMS_ACTION.equals(intent.getAction())) {
                IInjectedComms comms = IInjectedComms.Stub.asInterface(
                        intent.getBundleExtra(InjectedMain.INTENT_COMMS_EXTRA_BUNDLE)
                                .getBinder(InjectedMain.INTENT_COMMS_EXTRA_BINDER)
                );
                uiLog("(binder) Binder received from target");
                try {
                    // This executes getProcessPackage() defined in injected.Main and returns the result
                    uiLog("(binder) Package name: " + comms.getProcessPackage());
                } catch (RemoteException e) {
                    uiLog("(binder) Package name: EXCEPTION");
                }
            }
        }
    };

    private TextView textView = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        textView = findViewById(R.id.textView);
        textView.setHorizontallyScrolling(true);
        textView.setMovementMethod(new ScrollingMovementMethod());

        try {
            // Show logcat data on-screen; this shell isn't properly closed so may error on app terminate
            android.util.Log.i("InjectVM/Start", UUID);
            Shell.Threaded shell = Shell.Pool.SU.get(new Shell.OnShellOpenResultListener() {
                @Override
                public void onOpenResult(boolean success, int reason) {
                    if (!success) showNoRootMessage();
                }
            });
            shell.addCommand("logcat", 0, new Shell.OnCommandLineListener() {
                private boolean haveUUID = false;

                @Override
                public void onSTDOUT(String line) {
                    if (line.contains("InjectVM/")) {
                        if (!haveUUID) {
                            haveUUID = line.contains(UUID);
                        } else if (line.contains("InjectVM/")) {
                            uiLog("(logcat) %s", line.substring(line.indexOf("InjectVM/") + "InjectVM/".length()));
                        }
                    } else if (haveUUID && line.contains(" F DEBUG ")) {
                        uiLog("(logcat) %s", line.substring(line.indexOf(" F DEBUG ") + " F DEBUG ".length()));
                    }
                }

                @Override public void onSTDERR(String line) {}
                @Override public void onCommandResult(int commandCode, int exitCode) {}
            });
        } catch (Shell.ShellDiedException e) {
            showNoRootMessage();
        }

        registerReceiver(commReceiver, new IntentFilter(InjectedMain.INTENT_COMMS_ACTION));
    }

    @Override
    protected void onDestroy() {
        try {
            unregisterReceiver(commReceiver);
        } catch (Exception e) {
            // why not throw an exception during cleanup, so useful
        }
        super.onDestroy();
    }

    private void uiLog(final String format, final Object... args) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                textView.append(String.format(Locale.ENGLISH, format + "\n", args));
            }
        });
    }

    public void showNoRootMessage() {
        (new AlertDialog.Builder(this))
                .setMessage("Could not acquire root")
                .setNegativeButton(android.R.string.ok, null)
                .show();
    }

    public void onPurpleSettingsClick(View view) {
        (new InjectTask(this)).execute("com.android.settings");
    }

    public void onSystemServerClick(View view) {
        (new InjectTask(this)).execute("system_server");
    }

    public static class InjectTask extends AsyncTask<String, Void, Boolean> {
        private WeakReference<MainActivity> activity;
        private ProgressDialog dialog = null;

        InjectTask(MainActivity context) {
            this.activity = new WeakReference<>(context);
        }

        @Override
        protected void onPreExecute() {
            MainActivity act = activity.get();
            if (act != null) {
                dialog = new ProgressDialog(act);
                dialog.setProgressStyle(ProgressDialog.STYLE_SPINNER);
                dialog.setMessage("Executing...");
                dialog.show();
            }
        }

        private int getPid(String target) throws Shell.ShellDiedException {
            ArrayList<String> STDOUT = new ArrayList<>();
            Shell.Pool.SU.run("(toolbox ps; toolbox ps -A; toybox ps; toybox ps -A) | grep \" " + target + "$\"", STDOUT, null, false);
            for (String line : STDOUT) {
                line = line.trim();
                while (line.contains("  ")) line = line.replace("  ", " ");
                String[] parts = line.split(" ");
                if (parts.length >= 2) {
                    return Integer.parseInt(parts[1]);
                }
            }
            return -1;
        }

        @Override
        protected Boolean doInBackground(String... strings) {
            final MainActivity act = activity.get();
            if (act != null) {
                try {
                    String target = strings[0];
    
                    // Note, if you are injecting into a non-system app, you may need to adjust
                    // policies so the target app can reach the payload, or place (and chcon) the
                    // payload in a different location. /data/app/.../lib/... seems to work, but
                    // only if the target app actually has native libraries of its own.
    
                    // Get native executable and library paths, and copy payload
                    String injector = act.getApplicationInfo().nativeLibraryDir + File.separator + "libinjectvm.so";
                    String payload_source = act.getApplicationInfo().nativeLibraryDir + File.separator + "libpayload.so";
                    String payload_dest = "/dev/injectvm_" + UUID + ".so";
                    act.uiLog("payload --> %s", payload_dest);
    
                    // Contexts differ between Android versions
                    String context = "u:object_r:system_lib_file:s0";
                    List<String> STDOUT = new ArrayList<>();
                    Shell.Pool.SU.run("ls -lZ /system/lib/libandroid_runtime.so", STDOUT, null, false);
                    for (String line : STDOUT) {
                        if (line.contains(" u:object_r:") && line.contains(":s0 ")) {
                            context = line.substring(line.indexOf("u:object_r:"));
                            context = context.substring(0, context.indexOf(' '));
                        }
                    }
    
                    // Copy, chmod, chcon
                    Shell.Pool.SU.run(new String[] {
                            "cp " + payload_source + " " + payload_dest,
                            "chmod 0644 " + payload_dest,
                            "chcon " + context + " " + payload_dest
                    });
    
                    // Kill and restart target (if app-style process) for repeatability
                    if (target.contains(".")) { // Naive Process == packageName ?
                        // Kill the process if it's already running
                        int pid = getPid(target);
                        if (pid >= 0) {
                            act.uiLog("%s: killing pid %d", target, pid);
                            Shell.Pool.SU.run("kill -9 " + pid);
                            try {
                                // Give it some time to die
                                Thread.sleep(1000);
                            } catch (Exception e) {
                                // no action
                            }
                        }
    
                        // Restart it
                        act.uiLog("%s: starting", target);
                        act.startActivity(act.getPackageManager().getLaunchIntentForPackage(target));
    
                        // Give package some time to start up
                        try {
                            Thread.sleep(1000);
                        } catch (Exception e) {
                            // no action
                        }
                    }
    
                    // get pid for target
                    int pid = getPid(target);
                    if (pid < 0) {
                        act.uiLog("%s: pid NOT FOUND", target);
                        return true;
                    }
                    act.uiLog("%s: pid %d", target, pid);
    
                    // inject
                    String command = String.format(Locale.ENGLISH,
                            "%s %d %s %s:%s:%s",
                            injector,
                            pid,
                            payload_dest,
                            act.getPackageCodePath(),
                            InjectedMain.class.getCanonicalName(),
                            "OnInject"
                    );
                    act.uiLog(command);
                    Shell.Pool.SU.run(command, new Shell.OnSyncCommandLineListener() {
                        @Override
                        public void onSTDERR(String line) {
                            act.uiLog("(stdout) %s", line);
                        }
    
                        @Override
                        public void onSTDOUT(String line) {
                            act.uiLog("(stderr) %s", line);
                        }
                    });
    
                    return true;
                } catch (Shell.ShellDiedException e) {
                    return false;
                }
            }
            return false;
        }

        @Override
        protected void onPostExecute(Boolean result) {
            dialog.dismiss();

            if (!result) {
                MainActivity act = activity.get();
                if (act != null) {
                    act.showNoRootMessage();
                }
            }

            dialog = null;
            activity = null;
        }
    }
}
