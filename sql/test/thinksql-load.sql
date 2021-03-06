CREATE SCHEMA RESTAURANT AUTHORIZATION RESTAURANT;
CREATE TABLE MENU_ITEM_GROUP (
MENU_ITEM_GROUP_NO INTEGER NOT NULL,
MENU_ITEM_GROUP_NAME VARCHAR(20),
PRIMARY KEY (MENU_ITEM_GROUP_NO) );
CREATE TABLE MENU_ITEM (
MENU_ITEM_NO INTEGER NOT NULL,
MENU_ITEM_NAME VARCHAR(40),
MENU_ITEM_GROUP_NO INTEGER,
PRICE DECIMAL(6,2),
PRIMARY KEY (MENU_ITEM_NO),
FOREIGN KEY (MENU_ITEM_GROUP_NO)
REFERENCES MENU_GROUP (MENU_ITEM_GROUP_NO) );
CREATE TABLE "SERVER" (
SERVER_NO INTEGER NOT NULL,
SERVER_NAME VARCHAR(20),
PRIMARY KEY (SERVER_NO) );
CREATE TABLE CHECK_HEADER (
CHECK_NO INTEGER NOT NULL,
SERVER_NO INTEGER REFERENCES "SERVER",
START_TIME TIMESTAMP(0),
PRIMARY KEY (CHECK_NO) );
CREATE TABLE CHECK_DETAIL (
CHECK_NO INTEGER NOT NULL REFERENCES CHECK_HEADER,
LINE_NO INTEGER NOT NULL,
MENU_ITEM_NO INTEGER REFERENCES MENU_ITEM,
QTY DECIMAL(5),
PRIMARY KEY (CHECK_NO, LINE_NO) );
CREATE VIEW FULL_CHECK_DETAIL AS
SELECT *
FROM RESTAURANT."SERVER"
NATURAL JOIN RESTAURANT.CHECK_HEADER
NATURAL JOIN RESTAURANT.MENU_ITEM_GROUP
NATURAL JOIN RESTAURANT.MENU_ITEM
NATURAL JOIN RESTAURANT.CHECK_DETAIL;
GRANT SELECT ON MENU_ITEM_GROUP TO PUBLIC;
GRANT SELECT ON MENU_ITEM TO PUBLIC;
GRANT SELECT ON "SERVER" TO PUBLIC;
GRANT SELECT ON CHECK_HEADER TO PUBLIC;
GRANT SELECT ON CHECK_DETAIL TO PUBLIC;
GRANT SELECT ON FULL_CHECK_DETAIL TO PUBLIC;

COPY 7 RECORDS INTO MENU_ITEM_GROUP FROM stdin USING DELIMITERS ' ','\n';
1 Starter
2 Main Course
3 Side-dish
4 Dessert
5 Soft drink
6 Beer
7 Wine

COPY 22 RECORDS INTO MENU_ITEM FROM stdin USING DELIMITERS ' ','\n';
1 Soup of the Day 1 2.45
2 Samosas 1 1.95
3 Prawn Cocktail 1 2.95
100 Lasagne 2 4.95
101 Spaghetti Bolognese 2 1.95
102 Paella 2 6.95
103 Borsch 2 4.95
104 Irish stew 2 3.95
105 Kedgeree 2 4.99
106 Boeuf Bourgignone 2 7.95
107 Roast Beef and Yorkshire Pudding 2 6.45
108 Chicken Madras 2 5.45
109 Chicken Tikka Masala 2 5.95
200 Boiled Rice 3 1.95
201 Mashed Potato 3 1.45
300 Red Wine 7 2.1
301 White Wine 7 2.1
302 Guinness 6 2.99
303 Lager 6 1.85
304 Lemonade 5 1.2
305 Cola 5 1.2
401 Ice Cream 4 2.99

COPY 2 RECORDS INTO "SERVER" FROM stdin USING DELIMITERS ' ','\n';
1 John Smith
2 Mary Jones

COPY 5 RECORDS INTO CHECK_HEADER FROM stdin USING DELIMITERS ' ','\n';
1 1 2001-01-12 11:55:12
2 1 2001-01-12 12:07:34
3 1 2001-01-12 13:24:39
4 2 2001-01-12 13:45:17
5 1 2001-01-12 13:46:09

COPY 22 RECORDS INTO CHECK_DETAIL FROM stdin USING DELIMITERS ' ','\n';
1 1 3 1
1 2 104 1
1 3 300 1
2 1 2 1
2 2 108 2
2 3 200 2
2 4 303 1
2 5 304 1
3 1 100 1
3 2 106 1
3 3 201 1
3 4 301 2
4 1 107 2
4 2 302 1
4 3 401 1
5 1 106 1
5 2 107 1
5 3 201 1
5 4 200 1
5 5 300 2
5 6 304 2
5 7 401 2

commit;
