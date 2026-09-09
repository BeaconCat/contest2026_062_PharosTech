# ABI2拓扑

N-Boot当前核为MPIDR 0。它通过Rockchip BL31 SIP配置非启动核Linux的EL2及
x0=最终DTB、x1..x3=0，再通过PSCI启动MPIDR 0x100。Linux初始化GIC与RPMsg，
启动器确认首个邮箱通知和接收描述符后，CPU0进入0x4a400000的openvela。

Linux自行启动0x101..0x103，openvela自行启动1..3。两边各有四个逻辑CPU，
但物理核不同。2026-09-10已通过真实health/info验证，见板测文档。

ABI2最终Linux DTB只能包含四个A72 CPU节点，均使用PSCI。status=disabled
不足以阻止此vendor内核的CPU枚举，因此删除A53节点及相关cpu-map/PMU引用。
旧单CPU3、ABI1和八CPU DTB不兼容，不能通过改FIT字段绕过检查。
