/* Copyright (c) 2019, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

package eu.chainfire.injectvm.injected;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.ArrayMap;
import android.view.View;
import android.view.ViewGroup;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import eu.chainfire.injectvm.BuildConfig;

// This class is injected into a different process and runs there
@SuppressWarnings({ "unused", "PrivateApi", "ConstantConditions", "FieldCanBeLocal" })
public class InjectedMain {
    private static InjectedMain instance = null; // prevent gc
    private static Handler mainThreadHandler = null;

    // This is called from a background thread in native code
    public static void OnInject() {
        mainThreadHandler = new Handler(Looper.getMainLooper());
        mainThreadHandler.post(new Runnable() {
            @Override
            public void run() {
                // run the rest of our code on the main thread
                instance = new InjectedMain();
            }
        });
    }

    public static final String INTENT_COMMS_ACTION = BuildConfig.APPLICATION_ID + ".COMMS";
    public static final String INTENT_COMMS_EXTRA_BUNDLE = INTENT_COMMS_ACTION + ".extra.bundle";
    public static final String INTENT_COMMS_EXTRA_BINDER = INTENT_COMMS_ACTION + ".extra.binder";

    private static void log(String format, Object... args) {
        android.util.Log.e("InjectVM/Target", String.format(Locale.ENGLISH, format, args));
    }

    private Class<?> cActivityThread;
    private Object currentActivityThread;
    private Binder comms = null; // prevent gc

    // ----- PURPLE SETTINGS -----

    private void makeViewPurple(View v, int level) {
        if (v == null) return;

        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < level; i++) {
            sb.append("--");
        }
        sb.append(v.getClass().getName());
        log(sb.toString());
        if (level == 0) {
            v.setBackgroundColor(0xFFFFFFFF);
        } else {
            v.setBackgroundColor(0x20FF00FF);
        }

        if (v instanceof ViewGroup) {
            ViewGroup vg = (ViewGroup)v;
            for (int i = 0; i < vg.getChildCount(); i++) {
                makeViewPurple(vg.getChildAt(i), level + 1);
            }
        }
    }

    private void makeActivityPurple(Activity activity) {
        log("Purpling activity");
        makeViewPurple(activity.getWindow().getDecorView().getRootView(), 0);
    }

    private void makeAppPurple() {
        try {
            // Get all active activities
            Field fActivities = cActivityThread.getDeclaredField("mActivities");
            fActivities.setAccessible(true);
            @SuppressWarnings("unchecked") ArrayMap<IBinder, Object> mActivities = (ArrayMap<IBinder, Object>)fActivities.get(currentActivityThread);

            log("Activities: %d", mActivities.size());

            // Make each activity purple
            Method mGetActivity = cActivityThread.getMethod("getActivity", IBinder.class);
            for (IBinder key : mActivities.keySet()) {
                Activity activity = (Activity)mGetActivity.invoke(currentActivityThread, key);
                makeActivityPurple(activity);
            }

            // Make sure new activities are also purple
            Method mGetApplication = cActivityThread.getMethod("getApplication");
            Application application = (Application)mGetApplication.invoke(currentActivityThread);
            application.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
                @Override public void onActivityResumed(Activity activity) {
                    makeActivityPurple(activity);
                }

                @Override public void onActivityCreated(Activity activity, Bundle savedInstanceState) {}
                @Override public void onActivityStarted(Activity activity) {}
                @Override public void onActivityPaused(Activity activity) {}
                @Override public void onActivityStopped(Activity activity) {}
                @Override public void onActivitySaveInstanceState(Activity activity, Bundle outState) {}
                @Override public void onActivityDestroyed(Activity activity) {}
            });
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    // ----- SYSTEM SERVER (BinderJack) -----

    // Get a system service' IBinder
    @SuppressWarnings("SameParameterValue")
    private IBinder getService(String service) {
        try {
            Class<?> cServiceManager = Class.forName("android.os.ServiceManager");
            Method mGetService = cServiceManager.getMethod("getService", String.class);
            return (IBinder)mGetService.invoke(null, service);
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    // Is this the original IBinder that registered the service? (We don't want the proxy!)
    private boolean isOriginalBinder(IBinder binder) {
        return binder instanceof Binder;
    }

    // This class' onTransact will be called by the native Binder instead of the original Binder's
    @SuppressLint("UseSparseArrays")
    private static class BinderJack extends Binder implements IInterface {
        private final class JackedMethod {
            final int code;
            final String name;
            final Method method;

            private JackedMethod(int code, String name) {
                this.code = code;
                this.name = name;

                Method m = null;
                if (targetMethods != null) {
                    for (Method method : targetMethods) {
                        if (method.getName().equals(name)) {
                            // Could there be multiple methods with the same name? Worth checking.
                            m = method;
                            break;
                        }
                    }
                }
                method = m;
            }
        }

        private final Binder target;
        private final Method[] targetMethods;
        private final Boolean hardCodedTransactions; // null == unknown, true == hard-coded, false == AIDL-generated
        private final Map<Integer, JackedMethod> resolveCode = new HashMap<>();

        BinderJack(Binder target) throws RemoteException {
            this.target = target;
            this.targetMethods = target.getClass().getMethods();
            attachInterface(this, target.getInterfaceDescriptor());

            Boolean hardCoded = null;

            // Get methods and names for transaction codes the standard way as generated from AIDL:
            // static final int <baseClass>$Stub::TRANSACTION_methodName = <int>
            Class<?> cStub = null;
            try {
                cStub = Class.forName(getInterfaceDescriptor() + "$Stub");
                hardCoded = false; // AIDL-generated, hurrah for consistency
            } catch (Exception e) {
                // no action
            }
            if (cStub != null) {
                try {
                    for (Field field : cStub.getDeclaredFields()) {
                        String name = field.getName();
                        if (name.startsWith("TRANSACTION_")) {
                            name = name.substring("TRANSACTION_".length());
                            field.setAccessible(true);
                            int code = field.getInt(null);
                            resolveCode.put(code, new JackedMethod(code, name));
                        }
                    }
                } catch (Exception e) {
                    throw new RemoteException(e.getMessage());
                }
            } else {
                // Most hard-coded system services on older Android versions have this listed as:
                // int <interface>::METHOD_NAME_TRANSACTION = <int>
                Class <?> cInterface = null;
                try {
                    cInterface = Class.forName(getInterfaceDescriptor());
                } catch (Exception e) {
                    // no action
                }
                if (cInterface != null) {
                    try {
                        for (Field field : cInterface.getDeclaredFields()) {
                            String name = field.getName();
                            if (name.endsWith("_TRANSACTION")) {
                                name = name.substring(0, name.length() - "_TRANSACTION".length()).toLowerCase();
                                StringBuilder sb = new StringBuilder();
                                char last = ' ';
                                for (int i = 0; i < name.length(); i++) {
                                    char c = name.charAt(i);
                                    if (c != '_') {
                                        if (last == '_') {
                                            sb.append(("" + c).toUpperCase());
                                        } else {
                                            sb.append(c);
                                        }
                                    }
                                    last = c;
                                }
                                int code = field.getInt(null);
                                resolveCode.put(code, new JackedMethod(code, sb.toString()));
                                hardCoded = true;
                            }
                        }
                    } catch (Exception e) {
                        throw new RemoteException(e.getMessage());
                    }
                }
            }
            hardCodedTransactions = hardCoded;
        }

        @Override
        protected boolean onTransact(int code, @NonNull Parcel data, @Nullable Parcel reply, int flags) throws RemoteException {
            JackedMethod method = resolveCode.get(code);
            String methodName = (method == null ? null : method.name);
            if (method != null) {
                if (method.method != null) {
                    // log matching method signature (stripping 'public' and the class name)
                    log("BinderJack: %d: %s", code, method.method.toString().replace(target.getClass().getCanonicalName() + ".", "").replace("public ", ""));
                } else {
                    // signature not found, just log the name
                    log("BinderJack: %d: \"%s\" (unresolved)", code, methodName);
                }
            } else {
                // name not even found for this code
                log("BinderJack: %d: (unknown)", code);
            }

            // You can manipulate the data passed to the original Binder here. The exact data
            // expected can differ between implementations (for example Google vs Samsung), not
            // just between Android versions; Services tend to wrapped, and both the service class
            // and the wrapper are part of the framework, so changing these signatures is not an
            // issue for OEMs. It may be prudent to check the signatures through reflection
            // (assuming methods even exist).

            // If you want to change data, obtain() a new Parcel and copy/modify the data there,
            // as the memory backing the passed Parcel may be mapped read-only.

            // Catch some broadcasted intents; you could possibly automate this through reflection
            if ("broadcastIntent".equals(methodName)) {
                // broadcastIntent(IApplicationThread, Intent, ...)
                data.enforceInterface(getInterfaceDescriptor()); // always first
                IBinder caller = data.readStrongBinder(); // IApplicationThread

                // Parcelables are preceded by 0 for NULL or 1 for value when the code is generated
                // automatically from AIDL files. On Android pre-8.0 IActivityService is hard-coded
                // (hand coded even maybe?) and broadcastIntent contains the Intent immediately.
                if ((hardCodedTransactions != null) && (
                        hardCodedTransactions ||
                        (!hardCodedTransactions && (data.readInt() == 1))
                )) {
                    Intent intent = Intent.CREATOR.createFromParcel(data);
                    log("--> broadcastIntent(%s)", intent.toString());
                }
            }

            // Run the code from the original Binder
            return target.transact(code, data, reply, flags);
        }

        @Override
        public IBinder asBinder() {
            return this;
        }
    }

    private void binderJackActivityService() {
        try {
            // Double check we're in the process that defined the Context.ACTIVITY_SERVICE service
            IBinder activityService = getService(Context.ACTIVITY_SERVICE);
            log("ActivityService:%s original:%s", activityService.getClass().getName(), isOriginalBinder(activityService) ? "true" : "false");

            if (isOriginalBinder(activityService)) {
                // Hijack ActivityService (note that this trick only works with Binders
                // originally implemented in Java, not those in native code)

                // Binder for Activity Service
                Binder binder = (Binder)activityService;

                // Binder that will actually be called instead of Activity Service
                BinderJack binderJack = new BinderJack(binder);

                // Ensure JavaBBinder object is created and held by JavaBBinderHolder
                // (calls ibinderForJavaObject internally) for our Binders. These are only
                // created when first used.
                Parcel parcel = Parcel.obtain();
                parcel.writeStrongBinder(binder);
                parcel.writeStrongBinder(binderJack);
                parcel.recycle();

                // Explicitly cache the original Activity Service Binder in ServiceManager, to
                // prevent system_server-internal calls which could cast directly back to
                // ActivityManagerService (which is bad, but what Samsung definitely does)
                // getting our BinderJack and dieing. Doing this of course also has the
                // effect that we do not catch system_server-internal calls at all.
                // Not doing this will return our BinderJack instance from getService().
                Class<?> cServiceManager = Class.forName("android.os.ServiceManager");
                Field fCache = cServiceManager.getDeclaredField("sCache");
                fCache.setAccessible(true);
                @SuppressWarnings("unchecked") Map<String, IBinder> sCache = (Map<String, IBinder>)fCache.get(null);
                sCache.put(Context.ACTIVITY_SERVICE, activityService);

                // Swap out native JNI reference to original Binder with BinderJack's
                hijackJavaBinder(binder, binderJack);

                // For debugging, verify this is ActivityManagerService
                log(getService(Context.ACTIVITY_SERVICE).getClass().getName());
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private native void hijackJavaBinder(Binder target, Object replace);

    // ----- COMMON -----

    // Runs on the main thread
    public InjectedMain() {
        try {
            // currentActivityThread = ActivityThread.currentActivityThread();
            cActivityThread = Class.forName("android.app.ActivityThread");
            Method mCurrentActivityThread = cActivityThread.getMethod("currentActivityThread");
            currentActivityThread = mCurrentActivityThread.invoke(null);

            // String currentPackageName = currentActivityThread.currentPackageName();
            Method mCurrentPackageName = cActivityThread.getMethod("currentPackageName");
            final String currentPackageName = (String)mCurrentPackageName.invoke(currentActivityThread);
            log("Hello World! from %s", currentPackageName == null ? "<null>" : currentPackageName);

            // Application application = currentActivityThread.mInitialApplication
            Field fInitialApplication = cActivityThread.getDeclaredField("mInitialApplication");
            fInitialApplication.setAccessible(true);
            Application application = (Application)fInitialApplication.get(currentActivityThread);

            // Create a command-and-control Binder and send it back to the main app.
            // For reasons unknown it doesn't work if we put the Binder directly in extras, only
            // when we wrap it in a Bundle first.
            comms = new IInjectedComms.Stub() {
                @Override
                public String getProcessPackage() {
                    log("getProcessPackage() called");
                    return currentPackageName;
                }
            };
            Intent intent = new Intent(INTENT_COMMS_ACTION);
            intent.setPackage(BuildConfig.APPLICATION_ID);
            Bundle bundle = new Bundle();
            bundle.putBinder(INTENT_COMMS_EXTRA_BINDER, comms);
            intent.putExtra(INTENT_COMMS_EXTRA_BUNDLE, bundle);
            application.getApplicationContext().sendBroadcast(intent);

            // Target-specific action
            if ((currentPackageName != null) && currentPackageName.equals("com.android.settings")) {
                makeAppPurple();
            } else if (currentPackageName == null) { // system_server ?
                binderJackActivityService();
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
