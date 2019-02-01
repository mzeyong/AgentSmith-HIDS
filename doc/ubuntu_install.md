# ubuntu 安装使用方法

这里主要采用 `APT` 进行包管操作，可能不是最简的安装，有问题忘提出issue

``` bash
apt install make gcc libelf-dev linux-header-{your_kernel_version} linux-image-{your_kernel_version} 
git clone https://github.com/mzeyong/AgentSmith-HIDS/
cd syshook/LKM
make
```

其余的就参考点融那边的样例就行了

ubuntu 上安装成功测试样例

![poc](doc/ubuntuproof.png)



