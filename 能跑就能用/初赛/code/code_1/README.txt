1、安装依赖
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config

sudo apt-get install -y qtbase5-dev qtchooser qt5-qmake qtbase5-dev-tools
sudo apt-get install -y libopencv-dev
sudo apt-get install -y fonts-wqy-zenhei  
2、驱动
PCIE驱动你要自己打开，具体硬件设备在你手上 这个要你自己研究怎么打开，并且你要sudo chmod给对应节点赋能

3、
sudo chmod +x ./*.sh
./rebuild.sh ---> 编译
编译完就可以./build/vision_qt_demo