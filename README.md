# Android VM injection and BinderJacking

Example code to inject and run Java(/Kotlin) code from your
own package (or dex) in any Android JVM process, including 
`system_server`. This part should be relatively stable and easily
reusable.

Also includes example code to hijack/hook ("BinderJack") a system service's 
`Binder` (`ACTIVITY_SERVICE` in this specific case) after injecting into
`system_server`. This part is not very stable, and would require
extensive testing to be generally usable cross-OEM and cross-Android
versions.

*Basic* testing has been done on various Google and Samsung devices 
running Android 7.0 through Q Preview 1. This code is published only
to show the possibilities, if you need production-quality code and
testing, *you*'ll have to do the work. There's a non-zero chance this
will not work out-of-the-box on *random Android device* you're testing 
with.

**Root is obviously required**. There is no typical exploiting code to
be found here. This is just a write-up of interesting techniques to use
when root is already available, and with the user's permission.

The example code logs both in the app and in logcat. Monitor both.

## License

Please see the [LICENSE](LICENSE) file for the exact details.

In summary:

Original ARM native code injector: Simone *evilsocket* Margaritelli's [ARM Inject](https://github.com/evilsocket/arminject) (&copy; 2015, BSD 3-clause).

Injector improvements, VM injection and everything Java-related: Jorrit *Chainfire* Jongma (&copy; 2015-2019, BSD 3-clause).

Excerpts from The Android Open Source Project (&copy; 2008, APLv2).

Credits are always appreciated if you use my code.

## Spaghetti Sauce Project

This release is part of the [Spaghetti Sauce Project](https://github.com/Chainfire/spaghetti_sauce_project).

## Native code injection

If you're reading this you're probably familiar with native code
injection: running your own native code inside a different process. 
There are various ways to accomplish this, all with their pros and cons.

As both my hobby and professional work tends to resolve around systems
where we already have significant access, but need to control/manipulate
processes that we can't easily replace or don't have up-to-date sources
for, my go-to methods are one of two:

Linker pre-loading (such as LD_PRELOAD) is an easy mechanism to get your
code (in the form of a shared library) loaded, and makes it trivial to 
hijack calls to libraries, but you need to have control over a process'
environment and its startup.

Ptrace-based injection loads your code (again in the form of shared
library) into an existing process. Its main advantage over linker
pre-loading is that you do not need control over the target process'
environment or its startup, but major disadvantages include that your
code isn't running from the start - state is hard to predict - and
you cannot rely on the linker to hijack library calls for you. The
latter is still possible, but you need to manipulate the GOT/PLT tables
manually.

If you're looking for code that does the latter, the sources for
[CF.lumen's performance driver](https://github.com/Chainfire/inject-hook-cflumen)
include it for Android, though it's a minefield and relatively unstable;
almost every major Android release has required adjustments, and at
the time of this writing there are still some issues with the latest 
Android version.

This project used ptrace-based injection, using a slightly modified
version of the injector from [that](https://github.com/Chainfire/inject-hook-cflumen)
repo, but this project does not itself use any of the GOT/PLT hijacking.

As I never really explained in that release how the injector works,
let me recap it here:

#### ptrace-based injection

`Ptrace` is a *nix system call that allows one process to control
another; debuggers tend to be based on this facility. Using this system 
call requires special privileges and is not available to standard 
Android processes. 

The basic premise is simple:
- Attach to the target process
- Find the symbols for `calloc/free/dlopen` in the target process
- Call `calloc` in the target process to allocate some memory
- Copy the path to the payload library to the target process
- Call `dlopen` in the target process to load the payload library
- The code in the payload library now lives in the target process,
and by utilizing a shared library constructor (or a manual call from
the injector), may start executing
- Cleanup

The `ptrace` call itself can be quite tricky and unpredictable to work
with, but the injector as it is now seems to work pretty reliably.

`Ptrace` works on a very low level. While it does provide the
functionality to read and write the target process' memory, executing
function calls has to be done by manipulating the stack and registers,
which means the code to do so differs between architectures (and 
potentially OS). That part of the injector's code currently supports
Android on arm, aarch64, i386, x86_64 - though not all of these have
necessarily been tested recently.

Locating the symbols to use in the target process is conceptually
simple. First we determine the symbol in the injector's memory,
then we examine `/proc/<pid>/maps` of both the injector and target
process. The symbol's offset from the base load address of the library
that defines the symbol is the same, so we can just do the math.

A minor snag can be locating the symbol in the injector's memory in the
first place. This tends to be done by using `dlopen` to (re)load the
library that defines the symbol, then using `dlsym` to resolve it. But
symbols aren't always defined in the same place. For example, on
Android `dlopen` itself is sometimes exported by the linker (in which
case you don't `dlopen`, just `dlsym`), and sometimes exported by 
`libdl.so`. In addition, the location of those changes as well.
Currently the possibilities for its location include 
`/system/bin/linker[64]`, `/system/lib[64]/libdl.so`, 
`/bionic/bin/linker[64]` and `/bionic/lib[64]/libdl.so`.

#### Linker namespaces

[Linker namespaces](https://source.android.com/devices/architecture/vndk/linker-namespace)
were introduced recently to Android, allowing separation of symbols
between libraries, and restrictions which libraries may be loaded by
which parts of the code.

The latter is also relevant to injecting our payload library, the
location must be picked carefully. For our purposes, putting the library
in `/dev` and `chmod` and `chcon` it accordingly seems to work well
enough.

Note that placing the payload library in the `/lib` directory of the
target process' APK (if any) also often does the trick, providing the
APK has native libraries of its own (otherwise the path is usually not 
added to the list of allowed paths).

There is an interesting work-around for linker namespaces (namely, 
creating your own) documented [here](https://fadeevab.com/accessing-system-private-api-through-namespace/),
but it is not used for this project.

## VM injection

Native code injection is great as it is, but the target process may be
running a VM (as is the case with most Android apps and even
`system_server`). Manipulating the internal states of such a process with
just native code can be tricky.

VM injection builds on top of the native code injection outlined 
earlier. The added trick is getting a reference to the active VM's 
`JavaVM` instance. After that you can obtain a `JNIEnv` and manipulate
the VM any way you want with native code, or load your own JVM classes 
into the target process to make all of that much easier. 

#### Retrieving JavaVM:

Conveniently, Android provides a static method to retrieve it: 
`JavaVM* android::AndroidRuntime::getJavaVM()`, which lives in 
`libandroid_runtime.so`. Unfortunately, this method is
not exported, but we can still access the static variable it returns
directly: `JavaVM* android::AndroidRuntime::mJavaVM`.

In older Android versions, this symbol can be easily resolved from
inside the payload library. Around 7.0, it started resolving to `NULL`. 
In my earlier investigations, I erroneously assumed ART had optimized it 
away (or something), but this has proven to be incorrect. I'm still not 
absolutely sure why this started happening, but linker namespaces seem 
an obvious answer.

The injector binary, when run from a properly rooted shell, does
not have any linker namespace restrictions or other potential voodoo
going on. We use the same trick inside the injector we used to resolve 
the `calloc/free/dlopen` symbols in the target's address space to 
resolve the symbol for `android::AndroidRuntime::mJavaVM`, then pass 
this address to the payload library by invoking one of its exported 
functions directly.

That trick is easy and only a few lines of code. Resolving the symbol
from the payload library itself may still be possible (using for 
example the linker namespace bypass linked earlier), but if you're going
to attempt it, let me inform you that hard-coded relative-to-`libandroid_runtime.so` 
addresses do not work. For example, in Android QP1 on PixelXL2 the pointer 
is the first few bytes of bss space after `libandroid_runtime.so`, while
in Android 7.0 on S7 the pointer resides in the rw- section of the
same library.     

NOTE: The VM injector has not been tested on older Android versions
where the `JavaVM` symbol should be able to be resolved in the easier
way, but it should not be difficult for you to adjust it if it doesn't
just work out-of-the-box.

#### Injecting our own JVM classes

Now that we have the `JavaVM`, we can get a `JNIEnv`, and reflection
our way through the VM, manipulating whatever we please, reading all
its data, using system services with the target process' identity, etc.

A lot of that can be done through JNI, but doing so is quite tedious.
It tends to be much easier doing these things from our own JVM classes,
so the next step is to load them.

`ClassLoader`s become the next problem. To load our classes, we need
one of these. We can't just use `ClassLoader::getSystemClassLoader()`
because this tends to return a `ClassLoader` that will only load classes
from `BOOTCLASSPATH`. Constructing a `ClassLoader` with the system
class loader as parent also doesn't work, as we'd be missing the
target process' classes.

The solution is to steal the current *Context* `ClassLoader`, which can
be obtained with `Thread.currentThread().getContextClassLoader()`. If
the target VM is fully loaded, this should refer to a `LoadedAPK`'s 
`ClassLoader` that has everything we want.

We then construct a `PathClassLoader` with the path to our own APK
(passed earlier to the payload by the injector from one of it's 
command line arguments), `loadClass` our injector class, and call
its entry-point through reflection. In this project that would
be `public static void eu.chainfire.injectvm.injected.InjectedMain::OnInject()`.

Et voila, now we have our own JVM code running inside the target 
process' VM.

#### Getting the goods

Even with our JVM classes injected, it is still not always trivial to
get at the data you're after to read or manipulate, or the instances
of the classes which methods you want to call.

Reflection and knowledge of Android internals will be your friend here.
In case you're targeting a specific app, you'll probably have to
decompile the target app and figure out the internals you're after.

Your main point of attack will always be singletons and static fields.
Luckily, Android itself is littered with them.

The `Purple Settings` option of the example app is good example of this.
It uses `android.app.ActivityThread::currentActivityThread()` to get the
main `ActivityThread`, and enumerates its `Activity`s through reflection,
then uses standard Android UI calls to walk through all the views
and turns their backgrounds purple.

You can get access to the `Application` object, `Service`s, etc, the
same way, which are obviously also `Context`s. You should be able to
get at any and all app-specific data and objects as they can always be 
reached through one of these (or other app-specific singletons), with 
sufficient knowledge of the internal workings of the target app.

#### IPC

As you'd only want to inject into a target process once, it is helpful
to set up IPC between the "parent" app and the injected process if 
more than run-once action is needed.

The example code does this by creating a `Binder` in the injected
process defined by `eu.chainfire.injectvm.injected.IInjectedComms.aidl`, 
then sends this to the parent app via Intent broadcast. The parent app 
can then easily call user-defined methods in the injected process. 
Isn't Binder just great?

#### Limitations

It should be explicitly noted that while this lets you inject and run 
arbitrary JVM code in a target process' VM, there is no method
call hooking functionality, like what is (or used to be) available
through frameworks such as *Xposed*.

#### Android Studio

Android Studio regularly fails to update the APK. Everything will
seem fine from the IDE, but old code is still executed on the device. 
Disabling *Instant Run* may help, otherwise doing a full rebuild usually
seems to fix the issue.

It is also advised to kill/restart any processes you've injected to 
between updates, though this isn't always necessary.

#### SafetyNet

Using this, even to inject into *system_server*, does not seem to break
SafetyNet on the devices I tested, at the time of writing.

## BinderJacking

Note: for this section, basic understanding of how the Binder mechanism
works is assumed.

Intercepting/hooking/manipulating Android service calls (and indeed 
other `Binder`s) can be useful for information gathering or changing 
functionality.

There are multiple ways to accomplish this with, with varying degrees
of success and compatibility. 

#### Method 1: IServiceManager.addService()

System services have a main registry, the Service Manager. 
Traditionally, there could be only one of these, but recent Android
versions have introduced separate service managers for HIDL and 
vendor services. We'll focus only on the main service manager here.

A system service's `Binder` is added to the registry by means of the 
`IServiceManager.addService()` call, and retrieved through 
`IServiceManager.getService()`, which are implemented in the
native `servicemanager` process. Ultimately, calls to 
`Context.getSystemService()` end up calling 
`IServiceManager.getService()`, or return an earlier retrieved
instance from a cache.

An interesting behavior of `IServiceManager.addService()` is that
it replaces an already registered service of the same name, no
questions asked.

With sufficient permissions (such as a root shell on a properly rooted
device has) you can add your own services. In fact, the *daemon*
variant of my [libRootJava](https://github.com/Chainfire/librootjava)
library currently uses this functionality to add it's own service,
and my (abandoned, source to be released) *FlashFire* app used it to
hijack OTA installs.

It is thus possible from a sufficiently privileged process to first 
retrieve a system service's `Binder` (Proxy), save a reference to it, 
and replace that service in the registry with its own `Binder`. In the
replacement `Binder.onTransact()` method you could then
investigate/manipulate the data, and (optionally) send it on to the
original service's `Binder`, to which you saved a reference earlier.

While this approach basically works and is easy to implement (note that
this example code uses a different method outlined later, not this one), 
there are some caveats that can make it incompatible with specific 
services or unpractical for your purposes:

###### Pre-existing processes

One issue is that pre-existing processes that have already retrieved
and stored a reference to the original service are not affected at all, 
their calls will keep being passed to the original service rather than
your replacement, in the same way that it is still possible for you
to call the original service with the earlier stored reference. You'd
have to kill/restart the processes for which you want to 
monitor/manipulate the service calls. 

###### Calling PID/UID

Another issue is that *if* your replacement service is running in a
different process than the original service, when you pass calls on 
to the original service, it will see your replacement service's PID
and UID as the client, rather than the original caller you are proxying
for. As various Android internals resolve information based on these
fields, this can present a major hurdle. 

The PID (`struct binder_transaction_data.sender_pid`) and UID fields
(`struct binder_transaction_data.sender_euid`) are filled by the Binder
driver inside the kernel, you have no influence on them from the
sending end.

As all cross-process Binder calls are communicated through `ioctl()`
calls in user-space, one solution is to inject native code into the
process that implements the original service, hijack the `ioctl()`
call, and manipulate `struct binder_transaction_data` as it is read
from the Binder kernel driver in that process. I have done this
(code not included here), it is certainly possible, and it does work.

#### Method 2: JavaBBinder.mObject reference swapping

Another method to do this (used in the code provided here) is swapping
out the `JavaBBinder.mObject` reference, though this only works for
`Binder`s implemented on the JVM.

Additionally, this method requires VM injection into the process that
implements the original service, and that the replacement/proxy `Binder`
is also implemented on the JVM, not natively.

I prefer this method over using `ServiceManager.addService()` because
both (usually) require injection to work properly anyway, and this one's
native code hacky bits are less complex and (I expect) more easily
updated to support potential changes in future Android version. Plus,
this method does not require any apps to be restarted.

###### Basic mechanism

When a call is made on a `Binder` interface in a client process,
the call is serialized and the data presented to the Binder kernel
driver. The kernel driver looks up which process is the server for
this interface, and presents the data to that process, including a 
previously associated pointer to the object that implements it.

That pointer (which to the best of my knowledge you are not able to
manipulate) in Android user-space references a (native) `BBinder` 
object. The code in `IPCThreadState` that reads this `BR_TRANSACTION` by
`ioctl()` from the Binder kernel driver ultimately calls the 
`BBinder.transact()` method of the pointed at object.

In case the `Binder` is implemented on the JVM, that native `BBinder` 
object is of the `JavaBBinder` subclass, which serves as the native 
wrapper to a JVM `Binder`. It holds a global reference to the JVM 
`Binder` subclass instance in the `JavaBBinder.mObject` field. The 
`JavaBBinder.onTransact()` method calls into the VM using JNI to
execute the `onTransact()` method of the JVM object pointed at
by that field.

As you have probably understood at this point, if you can change the 
value of `JavaBBinder.mObject`, you can change which JVM `Binder`
will be called to handle cross-process Binder calls for a service.

All we need to do is create our own JVM `Binder`, store a reference
to the original service `Binder`, and replace that native code
reference, then we have our proxy/hook able to monitor and manipulate
the service calls.

###### Getting at JavaBBinder

To be able to modify the reference, we (obviously) have to be in the 
process that implements the service. Due to how Binder internals work, 
this also conveniently avoids the *Calling PID/UID* issue described
earlier. 

Before we can modify the reference, we have to find it. Luckily,
the `JavaBBinder` object is owned by a `JavaBBinderHolder` object,
which in turn is owned by the JVM `Binder`. A native pointer to
the `JavaBBinderHolder` object is stored in JVM `Binder.mObject`.
We have a clear path to get to the reference once we obtain the
original JVM `Binder` that implements the service.

As we've injected into the actual process that defines the service,
`ServiceManager::getService()` conveniently returns the original
`Binder` object implementing the service, rather than the proxy you
would normally obtain, and thus JVM `Binder.mObject` is also easy
to obtain.

JVM `ServiceManager::getService()` --> JVM `Binder.mObject` --> native `JavaBBinderHolder.mBinder` --> native `JavaBBinder.mObject`

While this is all focused on BinderJacking system services at this
point, I see no reason why a similar technique wouldn't work for
hijacking an app-based AIDL-generated `Service`, though I have not
specifically tested it.

The actual implementation of modifying the reference is in the native:

###### hijackJavaBinder()

This function in the provided code is responsibly for actually
modifying the `JavaBBinderHolder.mObject` reference.

Unfortunately the Android framework implementations of the 
native `JavaBBinderHolder` and `JavaBBinder` classes differ slightly
between Android versions, and their symbols are not exported, so
finding and adjusting the reference is slightly fuzzy.

Fortunately, we know the reference is a *JNI Global Reference*, and 
`JNIEnv->GetObjectRefType()` and `JNIEnv->IsSameObject()` can help
us verify if a value in memory is what we're after.

This function is one of the most likely pieces of code to break between
Android versions. It's also likely to break (fixable) if multiple 
injectors try to hijack the same service.

Ultimately it's only a couple lines of code, so it should not be that
hard to adjust.

#### Casts and caching

As described earlier, `ServiceManager::getService()` returns the
original `Binder` implementing the service, when called inside the
process that implements it. Inside that process, it is thus possible to cast that `IBinder`
directly back to for example `ActivityManagerService` in case of
`Context.ACTIVITY_SERVICE`.

While I'm not aware of any AOSP code that explicitly does this,
Samsung code certainly does it on their devices.

I'm not aware of an easy way to counter this using the first BinderJack
method, but the provided code fixes this for the second method by
explicitly placing an instance of the real `ActivityManagerService` into
`ServiceManager::sCache`, making sure future calls of
`ServiceManager::getService()` return that `Binder`.

This also means that `system_server`-internal calls to 
`Context.ACTIVITY_SERVICE` will not pass through our proxy.

#### Manipulating the data

Now that we're able to reroute calls to a system service to our own 
`Binder`, what can we do with it?

`eu.chainfire.injectvm.injected.InjectedMain.BinderJack` provides
a simple sample implementation. The 
`onTransact(int code, Parcel data, Parcel reply, int flags)` 
method attempts to resolve the transaction's `code` to a method of 
the target interface (`IActivityService` in this case) and logs that,
before passing on the data to the original `Binder.transact()` method.

Parameters to the call and the return value are encoded into and passed
via `data` and `reply` `Parcel` parameters. You need to know the exact 
parameters of the wrapped call to be able to decode or manipulate the 
data correctly.

Keep in mind that both the services and the wrappers ultimately used by 
the SDK for system services reside inside the framework. This means OEMs 
can change the method signatures (and thus the passed data and its 
structure) on the `Binder` interface without breaking SDK contracts, 
*and they do*. It is quite possible that the data passed on the same
Android version for the same method on the same interface differs
between a random Pixel and a random Samsung. It is not very common, but
I wouldn't say it is exactly uncommon either.

You may be able to use reflection to verify a method exists on the 
target `Binder` with the signature you expect, but remember that a 
`Binder` implementation has no obligation to implement any methods at 
all - it just needs to provide a `Binder.onTransact()` method. That it
generally *does* implement these methods directly is an implementation
detail (that is blessedly stable when AIDL is used).

Pre-Android 8.0 Oreo, it seems many (if not all) system service
interfaces have hard-coded their `Parcel` reading and writing. Whether
this was done by hand or using some tool I do not know. Either way,
these `Binder`s do not necessarily follow convention. One example
elaborated on in the provided code is that for 
`IActivityService.broadcastIntent()`, there is no `int` preceding
the `Intent` data signifying if the `Intent` is `NULL` or not, as one
would normally expect with a `Parcelable`.

Since Android 8.0 Oreo, it seems system services have switched to
"standard"/consistent AIDL-based code generation. It should be 
feasible to write a class that parses/manipulates the `data` `Parcel` 
automagically based on the method signature found by reflection for 
these, though at this point I have not done so. Alternatively, you may 
convince the AIDL compiler to generate the relevant code for you, 
though obviously that cannot adjust to signature changes detected at 
runtime as a reflection-based manipulator could. 

Note that if you're going for data manipulation rather than just
monitoring, you want to `obtain()` a new `Parcel`, copy/modify the 
data there, and pass that `Parcel` to the real service; the buffer 
backing the passed `Parcel` may be mapped read-only. 

## Ideas (Mostly off-topic)

Ideas aplenty for these capabilities, from the obviously nefarious to 
the interesting and helpful. After all, you can get almost obscene 
levels of access to app's internals and data, and can even manipulate 
system service calls.

The reason I was originally interested in these techniques is this,
though:

#### Hiding (app-)root and changing it's interface

This idea goes back many years to early development of *SuperSU*,
and predates both *SafetyNet* and *Magisk*. As *SuperSU* no longer
really exists, and due to changes to how pretty much everything works 
under the hood in the (app-)root world, including how *Magisk* handles
the *su* binary, binds, and *SafetyNet*, most of this may no longer
be relevant. I wanted to write it down anyway, as these would (probably)
have been the final building blocks required to make it happen if this
were still actual. Better years late than never, eh?

###### su shell

The `su` command has always irked me a bit. It's an *nixism that was
copied to Android, where it's a bit out of place. By far most apps
are JVM based, and we're dropping into a shell and managing comms to 
execute Linux commands (which under the hood is complexer than it
seems) to do things that could usually be done pretty 
easily through the JVM itself. Since Android 4.3 the situation has
become a bit worse, as the `su` command no longer runs a real shell
directly, but becomes a proxy to an actual shell running as root
elsewhere in the process tree.

Over time and Android versions, some interesting gymnastics have had
to be performed to keep this command accessible to apps as well. Its
real location has moved repeatedly, usually ultimately bind mounting it 
into places where it was still accessible through `PATH`. I don't know
about *Magisk*, but in the *SuperSU* days a lot of work went into
keeping that part working.

Imagine if a `Binder` could have been used to communicate with the root
management app (requesting root by interface calls, with proper 
callbacks instead of timeouts) using simple JVM calls. Sprinkle in a
method call to run any of your JVM classes inside a root process
(using a Zygote-like forking parent construct) and a lot of things would 
have become much less complex to do, as well as cheaper processing-wise. 
Many shell commands issues by root apps are easier to do directly from 
the JVM as you wouldn't have to worry about varying busybox versions or
how they were symlinked, toolbox vs toybox, etc. (Granted these are
less of an issue these days, but they caused lots of issues in the
past.)

Many root apps (and *Magisk* modules) are glorified wrappers for a
few shell commands to be run as root (I know saying this angers some
devs as if I'm downplaying their efforts, I'm not, the assessment
is true nevertheless), and for those it would not make a whole lot of
difference, but complexer apps (and I had several of those myself)
would be much easier to write.

We could get rid of the `su` command and its requirements completely
that way. That doesn't mean you can't still run shell commands, but
once you'd have one of your JVM classes already running as root,
running a `sh` command from there is much cheaper than running `su`
from an unprivileged process.

A `su` command wrapper could still exist, such as for use from
*ADB shell* or scripts, but it wouldn't be the main path to root 
anymore. Striking it completely for root apps would be the goal, 
though it seems obvious such a move would not work beyond this 
thought exercise, as too much existing code depends on it.

Of course, these days we have multiple extensive libraries that already 
take care of most or all of these things and their complexities, such as
[libsuperuser](https://github.com/Chainfire/libsuperuser), 
[libRootJava](https://github.com/Chainfire/librootjava), 
and [libsu](https://github.com/topjohnwu/libsu), but at their core
they're still all relying on the `su` command.

Using a `Binder` interface as core would also give us kernel-backed
client UID and PID verification, fast IPC, and file descriptor
transfer built-in and essentially for free. At least in *SuperSU*'s 
case, this could also have cut down significantly on the complexity
of the root daemon.

###### Hiding it

Of course, we couldn't just add a service to the registry, as any app
could then just query the registry for the service and know if a device
is rooted. But with BinderJacking, we could piggy-back our `Binder`
interface onto an existing service. We'd just handle same additional
codes that aren't present in the original service's `Binder`.

If the client's UID should be hidden from, then those codes just
wouldn't reply, giving the exact same result on the client as if there
were no root at all.

As we would no longer be using the `su` command, various bind magics
could disappear completely, and testing for that shell command also
wouldn't work.

The GUI for the root management app could be loaded entirely inside
an existing framework app from a dex and run from there. There wouldn't 
be any *SuperSU* (or *Magisk*, or...) APK to detect, as it simply 
wouldn't exist. The GUI would then also gain priv-app privileges
directly, which could help with various management functionalities.

Of course this isn't an issue much these days anyway with the current
hiding techniques, but it was an issue back when I first thought of
this. 

###### Could it have worked?

Technically I think it could. If I would be writing a new root
management tool from scratch today, that is likely how I would do 
it.

Most of the core of the daemon would be running on the JVM itself,
instead of *SuperSU*'s C core, exposing its service through a `Binder` 
piggy-backed onto some system service. 

Binder's kernel-backed client UID/PID verification would be used to 
resolve requests to apps that do or don't have root, or from which it 
should be hidden.

Binder IPC would be used for requesting/granting root (and proper
callbacks instead of timeouts), executing shell commands (with
optional STDIN/STDOUT/STDERR file descriptor transfer), and
running any client's JVM class as root directly (which then in turn
can do anything it wants, including creating more shells using one of
those libraries). The class loading functionality would obviously also 
provide an easy way to transfer internal `Binder`s between the classes 
running as root and the client's non-privileged app for IPC, and run 
each client app'd root classes in a privately forked instance.

A `su` command, if existing still for compatibility, would then
convert to a script that calls into that service and executes a shell,
transferring its STDIN/STDOUT/STDERR descriptors to it. Much like how
the `am` and `pm` commands work. In newer Android versions, 
`Binder` even has built-in functionality to do all this (see how `pm`
works in Android 9.0 vs Android 7.0 for details).

The GUI would live inside Android's SystemUI or a similar package.

All of this would be loaded through a dex stored somewhere, with no
actual APK to be found anywhere.

I am not intimately familiar with *Magisk* internals, but I am confident
that this would have been a less complex solution, as well as cleaner
and probably faster (both in execution time as well as development time)
than how *SuperSU* was built.

The biggest problem would be on-boarding the developers of root-using
apps. Removing the `su` interface that everyone has been relying on
for a decade would be a very tough sell indeed.

###### Would you do it?

No. Aside from that I simply don't do those things anymore, root usage 
is dwindling fast, and most of the problems that would've
been solved by this approach aren't really much of a problem anymore
these days anyway.

The advantages to this approach would mostly be on my end, while 
developers of root-using apps can already accomplish most if not all of 
the advantages it would bring to them using existing libraries.  

Switching everything up just to have a backing interface that appeals to 
me personally at this time in Android is completely silly, *but* if I
had all the knowledge I have today back in 2012 or so, I might have had
a go at it.

As it is now, this is purely academic, but it was an interesting thought
to me anyway.
