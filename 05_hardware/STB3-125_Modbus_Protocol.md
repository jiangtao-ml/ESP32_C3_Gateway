<table style="width:97%;">
<colgroup>
<col style="width: 17%" />
<col style="width: 24%" />
<col style="width: 43%" />
<col style="width: 10%" />
</colgroup>
<tbody>
<tr>
<td style="text-align: center;"><strong>日期</strong></td>
<td style="text-align: center;"><strong>更新前</strong></td>
<td style="text-align: center;"><strong>更新后</strong></td>
<td style="text-align: center;"><strong>版本</strong></td>
</tr>
<tr>
<td style="text-align: center;"><strong>2020年9月11日</strong></td>
<td style="text-align: left;"><strong>参数设为0为关闭相关功能</strong></td>
<td style="text-align: left;"><p><strong>1对参数范围进行更改,为0时不关闭相关功能,加入保持寄存器15作为跳闸功能控制位开关;</strong></p>
<p><strong>2加入余额不足跳闸功能,保持寄存器14为余额限值</strong></p></td>
<td style="text-align: center;"><strong>V3.1</strong></td>
</tr>
<tr>
<td style="text-align: left;"><strong>2021年3月16日</strong></td>
<td style="text-align: left;"><strong>保持寄存器里有分合闸功能</strong></td>
<td style="text-align: left;"><strong>引起不必要问题，去除此功能</strong></td>
<td style="text-align: center;"><strong>V3.2</strong></td>
</tr>
<tr>
<td style="text-align: left;"><strong>2021年8月3日</strong></td>
<td style="text-align: left;"><strong>无寄存器编号不通用</strong></td>
<td style="text-align: left;"><strong>寄存器编号通用化</strong></td>
<td style="text-align: center;"><strong>V3.21</strong></td>
</tr>
<tr>
<td style="text-align: left;"><strong>2021年10月3日</strong></td>
<td style="text-align: left;"><strong>功能码：01，05，03，16</strong></td>
<td style="text-align: left;"><strong>加入功能码06功能</strong></td>
<td style="text-align: center;"><strong>V4.0</strong></td>
</tr>
<tr>
<td style="text-align: left;"><strong>2021年11月26日</strong></td>
<td style="text-align: left;"><strong>无远程锁死</strong></td>
<td style="text-align: left;"><strong>加入远程锁扣放在40012，本地锁扣放在30001高位</strong></td>
<td style="text-align: center;"><strong>V4.1</strong></td>
</tr>
</tbody>
</table>

**版本更新记录:**

**通信地址表**

**MODBUS地址信息表 版本号：V4.1**

<table style="width:92%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 11%" />
<col style="width: 18%" />
<col style="width: 31%" />
<col style="width: 20%" />
</colgroup>
<tbody>
<tr>
<td>地址</td>
<td>命令类型</td>
<td>功能描述</td>
<td>数据范围说明</td>
<td>说明</td>
</tr>
<tr>
<td colspan="5"><strong>读线圈操作</strong> 功能码: 读(0X01)</td>
</tr>
<tr>
<td>0（00001）</td>
<td>位状态线圈</td>
<td>当前电压故障</td>
<td>1:有 0:无</td>
<td></td>
</tr>
<tr>
<td>1（00002）</td>
<td>位状态线圈</td>
<td>远程合闸/分闸</td>
<td>1:合闸0:分闸</td>
<td></td>
</tr>
<tr>
<td>2（00003）</td>
<td>位状态线圈</td>
<td>远程锁扣/解锁</td>
<td>1:锁扣0:解锁</td>
<td></td>
</tr>
<tr>
<td>3（00004）</td>
<td>位状态线圈</td>
<td>自动控制/手动</td>
<td>1:自动0:手动</td>
<td></td>
</tr>
<tr>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
</tr>
<tr>
<td colspan="5">输出线圈操作 功能码: 写(0X05)(支持广播)</td>
</tr>
<tr>
<td>0（00001）</td>
<td>输出线圈</td>
<td>配置复位</td>
<td>0xFF00</td>
<td></td>
</tr>
<tr>
<td>1（00002）</td>
<td>输出线圈</td>
<td>远程合闸/分闸</td>
<td>0xFF00/0x0000</td>
<td></td>
</tr>
<tr>
<td>2（00003）</td>
<td>输出线圈</td>
<td>远程锁扣/解锁</td>
<td>0xFF00/0x0000</td>
<td></td>
</tr>
<tr>
<td>3（00004）</td>
<td>输出线圈</td>
<td>自动控制/手动</td>
<td>0xFF00/0x0000</td>
<td></td>
</tr>
<tr>
<td>4（00005）</td>
<td>输出线圈</td>
<td>记录清零</td>
<td>0xFF00</td>
<td></td>
</tr>
<tr>
<td colspan="5"><strong>保持寄存器</strong> 功能码: 多写(0X10) 单写（0X06） 多读(0X03)</td>
</tr>
<tr>
<td>0（40001）</td>
<td>保持寄存器</td>
<td>设备地址</td>
<td><p>高字节为子网地址</p>
<p>低字节为设备地址</p></td>
<td><p>默认00 01（0为广播）</p>
<p>子网和广播无回应</p></td>
</tr>
<tr>
<td>1（40002）</td>
<td>保持寄存器</td>
<td>波特率</td>
<td>1200 ~ 19200</td>
<td>默认为9600</td>
</tr>
<tr>
<td>2（40003）</td>
<td>保持寄存器</td>
<td>过压值</td>
<td>250 ~300 (V)</td>
<td>读写 复位后为275</td>
</tr>
<tr>
<td style="text-align: left;">3（40004）</td>
<td style="text-align: left;">保持寄存器</td>
<td style="text-align: left;">欠压值</td>
<td style="text-align: left;">150 ~200 (V)</td>
<td style="text-align: left;">读写 复位后为160</td>
</tr>
<tr>
<td>4（40005）</td>
<td>保持寄存器</td>
<td>过流值</td>
<td>1~100A(单位：0.01A)</td>
<td>读写 复位后为1000</td>
</tr>
<tr>
<td>5（40006）</td>
<td>保持寄存器</td>
<td>漏电值</td>
<td>10~90(单位：1mA)</td>
<td>读写 复位后为50</td>
</tr>
<tr>
<td>6（40007）</td>
<td>保持寄存器</td>
<td>接口过温值</td>
<td>40~150(℃)</td>
<td>读写 复位后为70</td>
</tr>
<tr>
<td>7（40008）</td>
<td>保持寄存器</td>
<td>过载有功功率</td>
<td>1000~50000(1W)</td>
<td>读写 复位后为2200</td>
</tr>
<tr>
<td>8（40009）</td>
<td>保持寄存器</td>
<td>限电度数H</td>
<td>1~999 999 999</td>
<td>读写 复位后为0</td>
</tr>
<tr>
<td>9（40010）</td>
<td>保持寄存器</td>
<td>限电度数L</td>
<td>(单位：0.001度)</td>
<td>读写 复位后为0</td>
</tr>
<tr>
<td>10（40011）</td>
<td>保持寄存器</td>
<td>客户数据H</td>
<td>0xFFFF</td>
<td>读写</td>
</tr>
<tr>
<td>11（40012）</td>
<td>保持寄存器</td>
<td>远程锁扣/解锁</td>
<td>0xFF00/0x0000</td>
<td>读写</td>
</tr>
<tr>
<td>12（40013）</td>
<td>保持寄存器</td>
<td>自动控制/手动</td>
<td>0xFF00/0x0000</td>
<td>读写(与对应线圈同步)</td>
</tr>
<tr>
<td>13（40014）</td>
<td>保持寄存器</td>
<td>远程合闸/分闸</td>
<td>0xFF00/0x0000</td>
<td>读写(与对应线圈同步)</td>
</tr>
<tr>
<td>14（40015）</td>
<td>保持寄存器</td>
<td>余额限值</td>
<td>10~50000(度)</td>
<td>读写 复位后为10</td>
</tr>
<tr>
<td>15（40016）</td>
<td>保持寄存器</td>
<td>跳闸开关</td>
<td>对应从低到高位:0,过压;1,欠压;2,过流3,漏电;4,过温;5,过载;6,费用功能开关;7,低余额跳闸开关</td>
<td><p>读写,每位单独定义1为开,0为关</p>
<p>复位后为全0</p></td>
</tr>
<tr>
<td colspan="5"><strong>输入寄存器</strong> 功能码(0X4)</td>
</tr>
<tr>
<td style="text-align: center;">0（30001）</td>
<td style="text-align: center;">输入寄存器</td>
<td style="text-align: center;">开关分/合闸状态</td>
<td><p>高字节：本地锁止：0x01,无锁：0</p>
<p>低字节：分闸：0xF,合闸：0xF0</p></td>
<td style="text-align: center;">只读</td>
</tr>
<tr>
<td style="text-align: center;">1（30002）</td>
<td style="text-align: center;">输入寄存器</td>
<td style="text-align: center;">分闸记录1</td>
<td><p>F：无</p>
<p>1：过流 2：漏电</p>
<p>3：过温 4：过载</p>
<p>5：过压 6：欠压</p>
<p>7：远程 8：模组</p>
<p>9：失压 B:限电</p></td>
<td><p>只读，XXXX=89AB</p>
<p>半字节为最小单位</p>
<p>8位记录最后九次分闸原因</p>
<p>9位记录前第十次分闸原因</p>
<p>A位记录前第十一次分闸原因</p>
<p>B位记录前第十二次分闸原因</p></td>
</tr>
<tr>
<td style="text-align: center;">2（30003）</td>
<td style="text-align: center;">输入寄存器</td>
<td style="text-align: center;">分闸记录2</td>
<td><p>F：无</p>
<p>1：过流 2：漏电</p>
<p>3：过温 4：过载</p>
<p>5：过压 6：欠压</p>
<p>7：远程 8：模组</p>
<p>9：失压 B:限电</p></td>
<td><p>只读，XXXX=4567</p>
<p>半字节为最小单位，0无</p>
<p>4位记录前第五次分闸原因</p>
<p>5位记录前第六次分闸原因</p>
<p>6位记录前第七次分闸原因</p>
<p>7位记录前第八次分闸原因</p></td>
</tr>
<tr>
<td style="text-align: center;">3（30004）</td>
<td style="text-align: center;">输入寄存器</td>
<td style="text-align: center;">分闸记录3</td>
<td><p>F：无</p>
<p>1：过流 2：漏电</p>
<p>3：过温 4：过载</p>
<p>5：过压 6：欠压</p>
<p>7：远程 8：模组</p>
<p>9：失压 B:限电</p></td>
<td><p>只读，XXXX=0123</p>
<p>半字节为最小单位，0无</p>
<p>0位记录最新一次分闸原因</p>
<p>1位记录前第二次分闸原因</p>
<p>2位记录前第三次分闸原因</p>
<p>3位记录前第四次分闸原因</p></td>
</tr>
<tr>
<td style="text-align: center;">4（30005）</td>
<td>输入寄存器</td>
<td>频率F</td>
<td>0~1000(单位：0.1Hz)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">5（30006）</td>
<td>输入寄存器</td>
<td>漏电电流L</td>
<td>0~1000(单位：1mA)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">6（30007）</td>
<td>输入寄存器</td>
<td>N相接线柱温度</td>
<td>0~200(单位：℃),-40后为实际温度</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">7（30008）</td>
<td>输入寄存器</td>
<td>A相接线柱温度</td>
<td>0~200(单位：℃),-40后为实际温度</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">8（30009）</td>
<td>输入寄存器</td>
<td>A相电压有效值U</td>
<td>0~600(单位：1V)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">9（30010）</td>
<td>输入寄存器</td>
<td>A相电流有效值I</td>
<td>0~0xFFFF(单位：0.01A)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">10（30011）</td>
<td>输入寄存器</td>
<td>A相功率因数Pf</td>
<td>0~100(单位：0.01)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">11（30012）</td>
<td>输入寄存器</td>
<td>A相有功功率P</td>
<td>0~0xFFFF(单位：1W)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">12（30013）</td>
<td>输入寄存器</td>
<td>A相无功功率Q</td>
<td>0~0xFFFF(单位：VAR)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">13（30014）</td>
<td>输入寄存器</td>
<td>A相视在功率S</td>
<td>0~0xFFFF(单位：VA)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">14（30015）</td>
<td>输入寄存器</td>
<td>A相有功电量高字H</td>
<td>1~999 999 999</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">15（30016）</td>
<td>输入寄存器</td>
<td>A相有功计量低字L</td>
<td>(单位：0.001度)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">16（30017）</td>
<td>输入寄存器</td>
<td>B相接线柱温度</td>
<td>0~200(单位：℃),-40后为实际温度</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">17（30018）</td>
<td>输入寄存器</td>
<td>B相电压有效值U</td>
<td>0~600(单位：1V)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">18（30019）</td>
<td>输入寄存器</td>
<td>B相电流有效值I</td>
<td>0~0xFFFF(单位：0.01A)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">19（30020）</td>
<td>输入寄存器</td>
<td>B相功率因数Pf</td>
<td>0~100(单位：0.01)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">20（30021）</td>
<td>输入寄存器</td>
<td>B相有功功率P</td>
<td>0~0xFFFF(单位：1W)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">21（30022）</td>
<td>输入寄存器</td>
<td>B相无功功率Q</td>
<td>0~0xFFFF(单位：VAR)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">22（30023）</td>
<td>输入寄存器</td>
<td>B相视在功率S</td>
<td>0~0xFFFF(单位：VA)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">23（30024）</td>
<td>输入寄存器</td>
<td>B相有功电量高字H</td>
<td>1~999 999 999</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">24（30025）</td>
<td>输入寄存器</td>
<td>B相有功计量低字L</td>
<td>(单位：0.001度)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">25（30026）</td>
<td>输入寄存器</td>
<td>C相接线柱温度</td>
<td>0~200(单位：℃),-40后为实际温度</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">26（30027）</td>
<td>输入寄存器</td>
<td>C相电压有效值U</td>
<td>0~600(单位：1V)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">27（30028）</td>
<td>输入寄存器</td>
<td>C相电流有效值I</td>
<td>0~0xFFFF(单位：0.01A)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">28（30029）</td>
<td>输入寄存器</td>
<td>C相功率因数Pf</td>
<td>0~100(单位：0.01)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">29（30030）</td>
<td>输入寄存器</td>
<td>C相有功功率P</td>
<td>0~0xFFFF(单位：1W)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">30（30031）</td>
<td>输入寄存器</td>
<td>C相无功功率Q</td>
<td>0~0xFFFF(单位：VAR)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">31（30032）</td>
<td>输入寄存器</td>
<td>C相视在功率S</td>
<td>0~0xFFFF(单位：VA)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">32（30033）</td>
<td>输入寄存器</td>
<td>C相有功电量高字H</td>
<td>1~999 999 999</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">33（30034）</td>
<td>输入寄存器</td>
<td>C相有功计量低字L</td>
<td>(单位：0.001度)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">34（30035）</td>
<td>输入寄存器</td>
<td>合相有功功率P</td>
<td>0~0xFFFF(单位：1W)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">35（30036）</td>
<td>输入寄存器</td>
<td>合相无功功率Q</td>
<td>0~0xFFFF(单位：VAR)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">36（30037）</td>
<td>输入寄存器</td>
<td>合相视在功率S</td>
<td>0~0xFFFF(单位：VA)</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">37（30038）</td>
<td>输入寄存器</td>
<td>合相有功电量高字H</td>
<td>1~999 999 999</td>
<td>只读</td>
</tr>
<tr>
<td style="text-align: center;">38（30039）</td>
<td>输入寄存器</td>
<td>合相有功计量低字L</td>
<td>(单位：0.001度)</td>
<td>只读</td>
</tr>
</tbody>
</table>

**电压读取**

01 04 00 08 00 01 B0 08

01 04 00 11 00 01 61 CF

01 04 00 1A 00 01 10 0D

**电流读取**

01 04 00 09 00 01 E1 C8

01 04 00 12 00 01 91 CF

01 04 00 1B 00 01 41 CD

**温度读取**

01 04 00 07 00 01 80 0B

01 04 00 10 00 01 30 0F

01 04 00 19 00 01 E0 0D

读取N相接线柱温度，N项为板载温度

01 04 00 06 00 01 D1 CB

读取全部参数

01 04 00 00 00 27 B0 10

***注:***

***1、默认标注类型为16位无符整型***

***2、锁扣状态下，开关不能合闸操作。***

***3、解锁状态下，开关能合闸操作。***

***产品出厂通信参数设置：***

设备地址： 1

波特率：9600

奇偶校验：无校验

CRC校验： 16位CRC校验，低位在前

停止位：1

**帧格式说明:**

1.  **写线圈**

**发送：**

<table style="width:54%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 9%" />
<col style="width: 10%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03 ~ 04</strong></td>
<td><strong>05 ~ 06</strong></td>
<td><strong>07 ~ 08</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码05</td>
<td><p>线圈</p>
<p>地址</p></td>
<td>操作码</td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**\
注：操作码定义为 FF 00**

**回应：**

<table style="width:54%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 9%" />
<col style="width: 10%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03 ~ 04</strong></td>
<td><strong>05 ~ 06</strong></td>
<td><strong>07 ~ 08</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码05</td>
<td><p>线圈</p>
<p>地址</p></td>
<td>操作码</td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

2.  **读线圈**

**发送：**

|              |          |          |             |             |             |
|--------------|----------|----------|-------------|-------------|-------------|
| **字节序号** | **01**   | **02**   | **03 ~ 04** | **05 ~ 06** | **07 ~ 08** |
| **功能**     | 设备地址 | 功能码01 | 起始地址    | 数据长度    | CRC校验     |

**\**

**回应：**

|              |          |          |              |              |             |
|--------------|----------|----------|--------------|--------------|-------------|
| **字节序号** | **01**   | **02**   | **03**       | **04**       | **05 ~ 06** |
| **功能**     | 设备地址 | 功能码01 | **字节长度** | **线圈状态** | CRC校验     |

3.  **读保持寄存器**

**发送：**

<table style="width:54%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 9%" />
<col style="width: 10%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03 ~ 04</strong></td>
<td><strong>05 ~ 06</strong></td>
<td><strong>07 ~ 08</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码03</td>
<td><p>寄存器</p>
<p>地址</p></td>
<td><p>数据</p>
<p>长度</p></td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**回应：**

<table style="width:75%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 8%" />
<col style="width: 12%" />
<col style="width: 13%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03</strong></td>
<td><strong>04 ~ 05</strong></td>
<td><strong>………</strong></td>
<td><strong>2n+2 ~ 2n+3</strong></td>
<td><strong>2n+4 ~ 2n+5</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码03</td>
<td><p>字节</p>
<p>长度</p></td>
<td>数据1</td>
<td>………</td>
<td><p>….</p>
<p>数据n</p></td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**注： 数据长度以2字节为1单位;数据长度不能超过16。**

4.  **读输入寄存器**

**发送：**

<table style="width:55%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 9%" />
<col style="width: 10%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03 ~ 04</strong></td>
<td><strong>05 ~ 06</strong></td>
<td><strong>07 ~ 08</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码04</td>
<td><p>寄存器</p>
<p>地址</p></td>
<td><p>数据</p>
<p>长度</p></td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**回应：**

<table style="width:76%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 8%" />
<col style="width: 12%" />
<col style="width: 13%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03</strong></td>
<td><strong>04 ~ 05</strong></td>
<td><strong>………</strong></td>
<td><strong>2n+2 ~ 2n+3</strong></td>
<td><strong>2n+4 ~ 2n+5</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码04</td>
<td><p>字节</p>
<p>长度</p></td>
<td>数据1</td>
<td>………</td>
<td><p>….</p>
<p>数据n</p></td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**注： 数据长度以2字节为1单位;数据长度不能超过16。**

5.  **写保持寄存器**

**发送：**

<table style="width:87%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 8%" />
<col style="width: 6%" />
<col style="width: 9%" />
<col style="width: 7%" />
<col style="width: 9%" />
<col style="width: 11%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03 ~ 04</strong></td>
<td><strong>05 ~ 06</strong></td>
<td><strong>07</strong></td>
<td><strong>08 ~ 09</strong></td>
<td><strong>…….</strong></td>
<td><strong>2n+6 ~ 2n+7</strong></td>
<td><p><strong>2n+8 ~</strong></p>
<p><strong>2n+9</strong></p></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码10</td>
<td><p>寄存器</p>
<p>地址</p></td>
<td><p>数据</p>
<p>长度</p></td>
<td><p>字节</p>
<p>长度</p></td>
<td>数据1</td>
<td>……</td>
<td><p>….</p>
<p>数据n</p></td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**回应；**

<table style="width:53%;">
<colgroup>
<col style="width: 11%" />
<col style="width: 6%" />
<col style="width: 6%" />
<col style="width: 8%" />
<col style="width: 8%" />
<col style="width: 10%" />
</colgroup>
<tbody>
<tr>
<td><strong>字节序号</strong></td>
<td><strong>01</strong></td>
<td><strong>02</strong></td>
<td><strong>03 ~ 04</strong></td>
<td><strong>05 ~ 06</strong></td>
<td><strong>07 ~ 08</strong></td>
</tr>
<tr>
<td><strong>功能</strong></td>
<td><p>设备</p>
<p>地址</p></td>
<td>功能码10</td>
<td><p>寄存器</p>
<p>地址</p></td>
<td><p>数据</p>
<p>长度</p></td>
<td>CRC校验</td>
</tr>
</tbody>
</table>

**调试用示例：**

读保持寄存器 0~7：（数量8个）

发送：01 03 00 00 00 08 44 0C

接收：01 03 10 00 01 25 80 00 00 00 00 00 00 00 00 FF FF FF FF FF E9

读保持寄存器 1~4：（数量4个） （超出的地址读为00）

发送：01 03 00 01 00 04 15 C9 //44 09

接收：01 03 08 25 80 01 13 00 A0 00 00 52 08

读保持寄存器 3：（数量1个）

发送：01 03 00 03 00 01 74 0A

接收：01 03 02 00 A0 B8 3C

写保持寄存器：

发送：01 10 00 00 00 04 08 00 01 25 80 0A BE 06 40 C1 FD

接收：01 10 00 00 00 04 C1 CA

说明： 修改保持寄存参数 通信地址，波特率，过压值，欠压值；

注意修改 通信地址，波特率后， 下次发送数据， 要做相应修改

合闸指令

发送：01 05 00 01 FF 00 DD FA

接送：01 05 00 01 FF 00 DD FA

01 05 00 01 FF 00 DD FA

发送：01 06 00 0D FF 00 59 F9

接送：01 06 00 0D FF 00 59 F9

分闸指令

发送：01 05 00 01 00 00 9C 0A

接送：01 05 00 01 00 00 9C 0A

远程锁扣命令：

发送：01 05 00 02 FF 00 2D FA

接收：01 05 00 02 FF 00 2D FA

发送：01 06 00 0D 00 00 18 09

接送：01 06 00 0D 00 00 18 09

远程解锁命令：

发送：01 05 00 02 00 00 6C 0A

接收：01 05 00 02 00 00 6C 0A

自动控制命令：

发送：01 05 00 03 FF 00 7C 3A

接收：01 05 00 03 FF 00 7C 3A

手动控制命令：

发送：01 05 00 03 00 00 3D CA

接收：01 05 00 03 00 00 3D CA

复位命令：

发送：01 05 00 00 FF 00 8C 3A

接收：01 05 00 00 FF 00 8C 3A

广播复位命令：

发送：00 05 00 00 FF 00 8D EB

读线圈状态 0~3：（数量4个）

发送：01 01 00 00 00 04 3D C9

回复：01 01 01 02 90 48

通过读输入寄存器获得A相电流值

发送：01 04 00 09 00 01 E1 C8

回复：01 04 02 00 0A 39 37
