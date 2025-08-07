git rev-parse --is-shallow-repository  # 返回 true 表示是浅克隆

git fetch --depth=1
git push -u origin vis2025 --force
# 或更安全的选项（避免覆盖他人提交）：
git push -u origin vis2025 --force-with-lease

git remote set-url origin https://gitee.com/hufz_admin/vis3588_kernel.git
git push -u origin vis2025 --force

https://github.com/kgp0213/vis3588_kernel.git

远程地址：
https://gitee.com/hufz/vis3588_ubuntu20.04.git
https://gitee.com/hufz/vis3588_uboot.git
https://gitee.com/hufz/vis3588_kernel.git

也可以写成下面的样式，可以实现ssh 密钥登陆，不用每次输入密码
git@gitee.com:hufz/vis3588_ubuntu20.04.git
git@gitee.com:hufz/vis3588_uboot.04.git
git@gitee.com:hufz/vis3588_kernel.04.git

修改uboot kernel rootfs远程仓库地址
# 这种是新增一个远程地址
git remote add github https://github.com/kgp0213/vis3588_kernel.git         
# 这种是替换远程地址
git remote set-url origin https://gitee.com/hufz/vis3588_ubuntu20.04.git

git remote set-url origin       git@gitee.com:hufz/vis3588_uboot.04.git
git remote add origin           git@gitee.com:hufz/vis3588_kernel.04.git
git remote set-url origin       git@gitee.com:hufz/vis3588_ubuntu20.04.git

验证远程地址是否已更改
git remote -v

由于是浅克隆，需要先解除浅克隆限制
git fetch --depth=1  # 确保只保留最近的历史（可选）,这样可以减少本地空间占用

git push -u origin vis2025  #推送本地分支到origin对应的仓库中

# 或更安全的选项（避免覆盖他人提交）：
git push -u origin vis2025 --force-with-lease
#强制推送
git push -u origin vis2025 --force  

