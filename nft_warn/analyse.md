怀疑补丁集：
047e813324eac2ac60cddfb58bcdbd0144eadb09 netfilter: nft_set_pipapo: don't leak bad clone into future transaction
744dc9a47a8458ed49becd6123a092c7dae82b8b netfilter: nft_set_pipapo: move cloning of match info to insert/removal path
fad1685df350faed5927c33e991df8c9d948ae20 netfilter: nft_set_pipapo: prepare pipapo_get helper for on-demand clone
840daa6bc4d12a57533d9bbcf348469bc15aa0b4 netfilter: nft_set_pipapo: merge deactivate helper into caller
8e264996d95f9b897e95c92ba0d71ff0e3246482 netfilter: nft_set_pipapo: prepare walk function for on-demand clone
7583b0d84ca272dc2e183f83b5395f5061444efa netfilter: nft_set_pipapo: make pipapo_clone helper return NULL
dde6b54848a5169baac365ef6ac1166899ac2396 netfilter: nft_set_pipapo: move prove_locking helper around
385e2a9360cc6edf9562cf8abcc4a0f18c199890 netfilter: nft_set_pipapo: use GFP_KERNEL for insertions

问题信息：
社区LTS分支6.1 5.15 6.6均出现此问题，但6.11和主线没出现
参考社区syzkaller：https://syzkaller.appspot.com/bug?extid=b338ce808595248a409a

本地仓库分支：
6.6：linux-6.6.y
主线: master
