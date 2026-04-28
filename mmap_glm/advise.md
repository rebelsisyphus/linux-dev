While one can argue something can be done to reduce the lock hold
time, the repeated acquire is a known pattern putting a low ceiling on
achievable scalability.

Regardless of what kind of data structure is used to represent these
mappings, the state needs to get decentralized in some capacity.

One option would be that mm_structs with mapppings to backed by a
given inode add themselves to a list in that inode. Then the repeat
calls only need to concern themsleves with modifying per-mm tracking.
The problem here is that there can be thousands of processes and
walking the mappings will become impractical.

Another option is to distribute the tree per-cpu. This again can be a
problem on bigger boxen and I'm not confident is all that warranted.

imo the a perfectly sensible way out is to merely distribute the state
with one instance per -- say -- 8 CPUs -- this would be a tradeoff
between scalability and the total count of nodes to visit when
walking.

I think it would also make sense to make it dynamic. For example start
with the current centralized state and trylock on addition. If
trylocks go past a threshold, convert it to the distributed state.
Then future additions/removals are largely deserialized, while
comparatively rarely used binaries don't use extra memory.

This is a rough outline for someone interested, maybe someone will
have a better idea. Extra points for going through with it. ;)
