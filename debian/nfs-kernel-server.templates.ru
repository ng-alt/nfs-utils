Template: nfs-kernel-server/tcpwrappers-mountd
Type: note
Description: in /etc/hosts.{allow,deny}, replace "rpc.mountd" with "mountd"
 The mount daemon uses tcpwrappers to control access.  To configure
 it, use program name "mountd" in /etc/hosts.allow and /etc/hosts.deny.
 .
 Older versions of nfs-kernel-server included a mount daemon that
 called itself "rpc.mountd".  Therefore, you should replace all
 occurrences of "rpc.mountd" with "mountd" in /etc/hosts.allow and
 /etc/hosts.deny.
Description-ru: �������� � /etc/hosts.{allow,deny} "rpc.mountd" �� "mountd"
 �����  ������������  ����������  ��� ���������� �������� tcp-��������.
 �����  ��  ���������,  �����������  ���  "mountd"  � /etc/hosts.allow �
 /etc/hosts.deny.
 .
 �����  ������  ������  nfs-kernel-server  �������� ����� ������������,
 �������  �������  ����  "rpc.mountd".  �������, �� ������ �������� ���
 ���������   "rpc.mountd"  ��  "mountd"  �  ������  /etc/hosts.allow  �
 /etc/hosts.deny.
