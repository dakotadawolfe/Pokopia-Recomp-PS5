# Service shared memory on PS5

`kernel::shared_memory_create(size, name, guest_permission)` allocates zeroed
shared backing and a stable private read/write service view. Logical sizes must
be nonzero multiples of 4 KiB. Guest permissions are explicitly read-only or
read/write. Service handles and mapped views each retain ownership; closing a
handle cannot free a live view. The service pointer never changes during map or
unmap, so readers and service updates observe the same pages without copying.

SVC 0x13 and 0x14 validate the complete logical size, object identity, exact
declared permission, guest region and native reservation. They preserve an
existing mapping on rejected requests. Failed permission publication removes
the new reservation and policy. Unmap removes only the matching object/view.
`svc_memory_reset_for_tests()` releases remaining views during quiescent
teardown, before the memory backend shuts down.

The size and permission checks follow the behavior in Atmosphere's
[shared-memory SVC implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_shared_memory.cpp)
and [shared-memory object](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_shared_memory.cpp),
reviewed 2026-11-04. This is a service-owned subset: guest CreateSharedMemory and
transfer-memory SVCs remain unimplemented.

PS5 direct mappings require a 16 KiB destination boundary. A destination aligned
only to 4 KiB currently returns InvalidCurrentMemory so the SDK address picker
can try another candidate. No copied-page fallback is reported as shared memory.
The unused end of a rounded host page is logically free with no guest access.
The generated-code subpage checks reject its reads/writes. The native padding
remains reserved until unmap, so another mapping cannot replace it even though
it is outside the guest object's logical range. Raw native pointers must respect the logical
length, since PS5 host permissions cover the full 16 KiB page.

The guest mappings use SubpageMap and are recognized by `mem::is_mapped()` even
though they are owned by kernel objects rather than the heap/fixed-reservation
backend. Guest memory reprotection does not change shared object permissions.
The stable service alias is private and must not be exposed as a guest pointer.

## Console validation

Build the existing `ps5_svc_tests` target. On 2026-11-04, console report
`svc-runtime-1793789794-390.log` completed **105 cases, zero failures**, and its
payload process exited. The executable was 3,391,080 bytes, SHA-256
`d50688c9a17be828bc22bee330eee7fba406015d6bcf823dfe476a3af1395399`.

The six new cases cover live updates across two mappings, guest-to-service
writes, end padding, invalid sizes/permissions/handles/addresses, collision
with an existing native reservation, injected protection failure with retry,
wrong-object unmap, duplicate handles and teardown after the final handle is
closed. The first run passed these cases but failed the older registration
inventory, which still expected the two newly ported SVCs to be absent. Updating
that inventory produced the complete passing run above. This payload evidence
does not establish native-app integration or completed game startup.

The real SDK subsequently exposed a guard-space mismatch in the first version:
`CheckGuardSpace` at `0x750014dcac` requires a free 16 KiB interval immediately
after the logical end, checked through QueryMemory at `0x7500151e18`.
Reporting rounded host padding as inaccessible made that check fail and caused
the SDK to unmap its successful 4 KiB clock view. These function addresses come
from the supplied SDK build `cc7da00d672399efe7958790d17fae396b1fde9f`; its
generated code remains private.

Padding is now logically free with no access, with the physical reservation and
subpage protection unchanged. The corresponding regression and twelve time
cases passed in **117 cases, zero failures**, report
`svc-runtime-1793790497-394.log`, executable SHA-256
`abd77a25c0643c8749b8291fd2cf692d8c3d67e3fd49a464243bdbb24406d499`.
The corrected SDK trace `sdk-startup-1793790561-395.log` retained the clock view,
passed both guard queries, and reached the unlinked `nninitStartup` import.
That trace is dependency discovery, not a successful game boot.
