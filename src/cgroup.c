#include "cgroup.h"
#include <unistd.h>
#include <smemory.h>
#include <async.h>
#include <errno.h>

typedef enum {
    CGROUP_CLOSED,
    CGROUP_OPEND
}qq_chat_group_state;

typedef struct qq_chat_group_
{
    qq_chat_group parent;
    PurpleLog* log;
    GList* msg_list;
    unsigned int unread_num;
    int state;
} qq_chat_group_;


static void force_open_dialog(qq_chat_group* cg)
{
    g_return_if_fail(cg);
    LwqqGroup* g = cg->group;
    PurpleChat* chat = cg->chat;
    PurpleConnection* gc = chat->account->gc;
    qq_account* ac = gc->proto_data;
    const char* key = try_get(g->account,g->gid);

    g_return_if_fail(key);
    PurpleConversation *conv = purple_find_chat(gc,opend_chat_search(ac,g));
    if(conv == NULL&&key) {
        serv_got_joined_chat(gc,open_new_chat(ac,g),key);
    }
}

static void set_user_list(qq_chat_group* cg)
{
    PurpleConversation* conv = CGROUP_GET_CONV(cg);
    qq_account* ac = cg->chat->account->gc->proto_data;
    LwqqClient* lc = ac->qq;
    LwqqSimpleBuddy* member;
    LwqqBuddy* buddy;
    PurpleConvChatBuddyFlags flag;
    GList* users = NULL;
    GList* flags = NULL;
    GList* extra_msgs = NULL;
    LwqqGroup* group = cg->group;

    PurpleConvChat* chat = PURPLE_CONV_CHAT(conv);
    //only there are no member we add it.
    if(purple_conv_chat_get_users(PURPLE_CONV_CHAT(conv))==NULL) {
        LIST_FOREACH(member,&group->members,entries) {
            extra_msgs = g_list_append(extra_msgs,NULL);
            flag = 0;

            if(lwqq_member_is_founder(member,group)) flag |= PURPLE_CBFLAGS_FOUNDER;
            if(member->stat != LWQQ_STATUS_OFFLINE) flag |= PURPLE_CBFLAGS_VOICE;
            if(member->mflag & LWQQ_MEMBER_IS_ADMIN) flag |= PURPLE_CBFLAGS_OP;

            flags = g_list_append(flags,GINT_TO_POINTER(flag));
            if((buddy = find_buddy_by_uin(lc,member->uin))) {
                if(ac->qq_use_qqnum)
                    users = g_list_append(users,try_get(buddy->qqnumber,buddy->uin));
                else
                    users = g_list_append(users,buddy->uin);
            } else {
                users = g_list_append(users,member->card?:member->nick);
            }
        }
        purple_conv_chat_add_users(chat,users,extra_msgs,flags,FALSE);
        g_list_free(users);
        g_list_free(flags);
        g_list_free(extra_msgs);
    }
    return ;
}

static void msg_free(PurpleConvMessage* msg)
{
    if(msg){
        s_free(msg->who);
        s_free(msg->what);
        s_free(msg);
    }
}

static void force_delete_log(PurpleLog* log)
{
    char procpath[128];
    char filepath[256]={0};

    PurpleLogCommonLoggerData* data = log->logger_data;
    int fd = fileno(data->file);
    if(fd<0) return;

    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d",fd);
    if(readlink(procpath,filepath,sizeof(filepath))<0) return;

    if(unlink(filepath)<0)
        lwqq_puts(strerror(errno));
}

qq_chat_group* qq_cgroup_new(struct qq_chat_group_opt* opt)
{
    qq_chat_group_ *cg_ = s_malloc0(sizeof(*cg_));
    cg_->parent.opt = opt;
    return (qq_chat_group*) cg_;
}

void qq_cgroup_free(qq_chat_group* cg)
{
    qq_chat_group_ * cg_ = (qq_chat_group_*) cg;
    if(cg){
        GList* ptr = cg_->msg_list;
        while(ptr){
            msg_free(ptr->data);
            ptr = ptr->next;
        }
        g_list_free(cg_->msg_list);
        purple_log_free(cg_->log);
    }
    s_free(cg);
}

void qq_cgroup_got_msg(qq_chat_group* cg,const char* serv_id,PurpleMessageFlags flags,const char* message,time_t t)
{
    qq_chat_group_ *cg_ = (qq_chat_group_*) cg;
    PurpleConnection* gc = cg->chat->account->gc;
    qq_account* ac = gc->proto_data;
    LwqqBuddy* b = find_buddy_by_uin(ac->qq, serv_id);
    LwqqSimpleBuddy* sb = NULL;
    const char* name;

    if(b == NULL ) sb = lwqq_group_find_group_member_by_uin(cg->group, serv_id);
    if(cg->group->mask>0&&CGROUP_GET_CONV(cg)==NULL){
        if(cg_->unread_num == 0){
            cg_->log = purple_log_new(PURPLE_LOG_CHAT, cg->group->account, cg->chat->account, NULL, t, NULL);
        }
        name = b?(b->markname?:b->nick):(sb?(sb->card?:sb->nick):serv_id);
        purple_log_write(cg_->log, flags, name, t, message);

        PurpleConvMessage *msg = s_malloc0(sizeof(*msg));
        msg->who = s_strdup(serv_id);
        msg->when = t;
        msg->flags = flags;
        msg->what = s_strdup(message);
        cg_->msg_list = g_list_append(cg_->msg_list,msg);

        cg_->unread_num ++;
        cg->opt->new_msg_notice(cg);
    }else{
        force_open_dialog(cg);
        set_user_list(cg);
        name = b?(b->qqnumber?:b->nick):(sb?(sb->card?:sb->nick):serv_id);
        serv_got_chat_in(gc, opend_chat_search(ac,cg->group), name, flags, message, t);
    }
}

void qq_cgroup_open(qq_chat_group* cg)
{
    force_open_dialog(cg);
    LwqqGroup* group = cg->group;
    qq_account* ac = cg->chat->account->gc->proto_data;
    PurpleConversation* conv = CGROUP_GET_CONV(cg);
    purple_conv_chat_set_topic(PURPLE_CONV_CHAT(conv), NULL, cg->group->memo);
    if(LIST_EMPTY(&group->members)) {
        LwqqCommand cmd = _C_(p,set_user_list,cg);
        LwqqAsyncEvent* ev = lwqq_info_get_group_detail_info(ac->qq,group,NULL);
        lwqq_async_add_event_listener(ev,cmd);
    } else {
        set_user_list(cg);
        //note only have got user_list, there may be unread msg;
        qq_chat_group_* cg_ = (qq_chat_group_*) cg;
        if(cg->group->mask>0&&cg_->unread_num>0){
            if(purple_log_delete(cg_->log)==0)
                force_delete_log(cg_->log);
            purple_log_free(cg_->log);
            cg_->log = NULL;

            GList* ptr = cg_->msg_list;
            while(ptr){
                PurpleConvMessage* msg = ptr->data;
                qq_cgroup_got_msg(cg, msg->who, msg->flags, msg->what, msg->when);
                msg_free(msg);
                ptr = ptr->next;
            }
            g_list_free(cg_->msg_list);
            cg_->msg_list = NULL;


            cg_->unread_num = 0;
            cg->opt->new_msg_notice(cg);
        }
    }
}


unsigned int qq_cgroup_unread_num(qq_chat_group* cg)
{
    return ((qq_chat_group_*)cg)->unread_num;
}
