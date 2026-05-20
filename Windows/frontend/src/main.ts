import { createApp } from "vue";
import ElementPlus from "element-plus";
import "element-plus/dist/index.css";
import "element-plus/theme-chalk/dark/css-vars.css";
import { MotionPlugin } from "@vueuse/motion";
import App from "./App.vue";
import router from "./router";
import "./style.css";

const app = createApp(App);
app.use(ElementPlus);
app.use(router);
app.use(MotionPlugin);
app.mount("#app");
