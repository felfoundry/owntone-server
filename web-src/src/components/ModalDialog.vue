<template>
  <div>
    <transition name="fade">
      <div v-if="show" class="modal is-active">
        <div class="modal-background" @click="$emit('close')" />
        <div class="modal-content fd-modal-card">
          <div class="card">
            <div class="card-content">
              <p v-if="title" class="title is-4" v-text="title" />
              <slot name="modal-content" />
            </div>
            <footer class="card-footer">
              <a class="card-footer-item has-text-dark" @click="$emit('close')">
                <span class="icon"><mdicon name="cancel" size="16" /></span>
                <span
                  class="is-size-7"
                  v-text="close_action ? close_action : t('dialog.cancel')"
                />
              </a>
              <a
                v-if="delete_action"
                class="card-footer-item has-background-danger has-text-white has-text-weight-bold"
                @click="$emit('delete')"
              >
                <span class="icon"><mdicon name="delete" size="16" /></span>
                <span class="is-size-7" v-text="delete_action" />
              </a>
              <a
                v-if="ok_action"
                class="card-footer-item has-background-info has-text-white has-text-weight-bold"
                @click="$emit('ok')"
              >
                <span class="icon"><mdicon name="check" size="16" /></span>
                <span class="is-size-7" v-text="ok_action" />
              </a>
            </footer>
          </div>
        </div>
        <button
          class="modal-close is-large"
          aria-label="close"
          @click="$emit('close')"
        />
      </div>
    </transition>
  </div>
</template>

<script>
export default {
  name: 'ModalDialog',
  props: ['show', 'title', 'ok_action', 'delete_action', 'close_action'],
  emits: ['delete', 'close', 'ok']
}
</script>

<style></style>
