import btlib

def send_msg(clis):
    ret = True
    ret = ret & btlib.connect_test(clis)
    ret = ret & btlib.jabber_login_test(clis)
    ret = ret & btlib.add_buddy_test(clis)
    ret = ret & btlib.message_test(clis)
    return ret

btlib.perform_test(send_msg)