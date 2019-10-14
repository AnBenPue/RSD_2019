## MES communication set up:   
Installing MySQL:
```bash
sudo apt install mysql-server -y
```
Checking installation
```bash
sudo systemctl status mysql.service
```
To see the current databases:
```bash
sudo mysqlshow -u root -p 
```
Accessing mysql prompt
```bash
 sudo mysql -u root -p 
```
Create new database (mysql prompt):
```
CREATE DATABASE rsd2018
```
 Importing the database structure (obtained from blackboard):
```bash
 sudo mysql -u root -p rsd2018 < ~/catkin_ws/src/Lego_bricks_packing/scripts/MES/db_export.sql
```
Create new user (mysql prompt):
```
use rsd2018
CREATE USER 'rsd'@'localhost';
GRANT ALL PRIVILEGES ON rsd2018.* TO 'rsd'@'localhost' IDENTIFIED BY 'rsd2018';
#source  ~/catkin_ws/src/Lego_bricks_packing/scripts/MES/db_export.sql
exit
```
Installing flask
```bash
pip3 install flask-mysql

```
Running the flask server:
```bash
python3 ~/catkin_ws/src/Lego_bricks_packing/scripts/MES/rsd_2019_app_public.py 
```
Running the requesting order script:
```bash
python3 ~/catkin_ws/src/Lego_bricks_packing/scripts/MES/ordering_client.py 
```
Possible problems:

* [ERROR 1819 (HY000): Your password does not satisfy the current policy requirements]
```bash
 # open mysql prompt:
 sudo mysql -u root -p
 # in the mysql prompt:
 uninstall plugin validate_password
```