<template>
  <section class="section fd-content">
    <div class="container">
      <div class="columns is-centered">
        <div class="column is-four-fifths">
          <section v-if="$slots['options']">
            <div ref="options_ref" style="height: 1px" />
            <slot name="options" />
            <nav
              class="buttons is-centered"
              style="margin-bottom: 6px; margin-top: 16px"
            >
              <a
                v-if="!options_visible"
                class="button is-small is-white"
                @click="scroll_to_top"
              >
                <span class="icon is-small"
                  ><mdicon name="chevron-down" size="16"
                /></span>
              </a>
              <a
                v-else
                class="button is-small is-white"
                @click="scroll_to_content"
              >
                <span class="icon is-small"
                  ><mdicon name="chevron-up" size="16"
                /></span>
              </a>
            </nav>
          </section>
          <div :class="{ 'fd-content-with-option': $slots['options'] }">
            <nav id="top" class="level">
              <!-- Left side -->
              <div class="level-left">
                <div class="level-item has-text-centered-mobile">
                  <div>
                    <slot name="heading-left" />
                  </div>
                </div>
              </div>
              <!-- Right side -->
              <div class="level-right has-text-centered-mobile">
                <slot name="heading-right" />
              </div>
            </nav>
            <slot name="content" />
            <div style="margin-top: 16px">
              <slot name="footer" />
            </div>
          </div>
        </div>
      </div>
    </div>
  </section>
</template>

<script>
export default {
  name: 'ContentWithHeading',

  data() {
    return {
      options_visible: false
    }
  },

  mounted() {
    if (this.$slots['options']) {
      this.observer = new IntersectionObserver(this.onElementObserved, {
        rootMargin: '-82px 0px 0px 0px',
        threshold: 1.0
      })
      this.observer.observe(this.$refs.options_ref)
    }
  },

  methods: {
    onElementObserved(entries) {
      entries.forEach(({ target, isIntersecting }) => {
        this.options_visible = isIntersecting
      })
    },

    scroll_to_top: function () {
      window.scrollTo({ top: 0, behavior: 'smooth' })
    },

    scroll_to_content: function () {
      if (this.$route.meta.has_tabs) {
        this.$scrollTo('#top', { offset: -140 })
      } else {
        this.$scrollTo('#top', { offset: -110 })
      }
    },

    visibilityChanged: function (isVisible) {
      this.options_visible = isVisible
    }
  }
}
</script>

<style></style>
